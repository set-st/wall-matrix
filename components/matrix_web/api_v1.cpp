// REST API, сумісний з AWTRIX NG (/api/v1/...), плюс бібліотека іконок.
//
// ESPHome реєструє в HTTP-сервері лише GET/POST/OPTIONS. PUT, PATCH і DELETE
// реєструємо самі на тому ж httpd і обробляємо тим самим handle_v1_().
#include "matrix_web.h"
#include "strip.h"

#include "esphome/core/application.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/components/json/json_util.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/scan_list.h"
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.api";

bool parse_informer(JsonObjectConst o, Informer &inf);  // informers.cpp
static constexpr size_t MAX_JSON_BODY = 4096;

// Доступ до захищеного конструктора запиту ESPHome
struct RawRequest : public AsyncWebServerRequest {
  explicit RawRequest(httpd_req_t *r) : AsyncWebServerRequest(r) {}
};

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------
static void jstr(std::string &out, const std::string &s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char) c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

static std::string error_json(const char *code, const std::string &msg) {
  std::string out = "{\"error\":{\"code\":\"";
  out += code;
  out += "\",\"message\":";
  jstr(out, msg);
  out += "}}";
  return out;
}

static bool valid_name(const std::string &n, size_t max_len = 32) {
  if (n.empty() || n.size() > max_len)
    return false;
  for (char c : n)
    if (!(isalnum((unsigned char) c) || c == '_' || c == '-'))
      return false;
  return true;
}

static std::string url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out += (char) strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Колір: "#RRGGBB", 0xRRGGBB (число) або [r, g, b]
static uint32_t parse_color(JsonVariantConst v, uint32_t def) {
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (*s == '#')
      s++;
    if (strlen(s) != 6)
      return def;
    char *end = nullptr;
    const uint32_t c = strtoul(s, &end, 16);
    return (end && *end == 0) ? c : def;
  }
  if (v.is<uint32_t>())
    return v.as<uint32_t>() & 0xFFFFFF;
  if (v.is<JsonArrayConst>()) {
    auto a = v.as<JsonArrayConst>();
    if (a.size() >= 3)
      return ((a[0].as<uint32_t>() & 0xFF) << 16) | ((a[1].as<uint32_t>() & 0xFF) << 8) | (a[2].as<uint32_t>() & 0xFF);
  }
  return def;
}

// Число з JSON: приймаємо й рядки ("20000") — шаблони HA інколи віддають так
static float num(JsonVariantConst v, float def) {
  if (v.isNull())
    return def;
  if (v.is<const char *>()) {
    char *end = nullptr;
    const float f = strtof(v.as<const char *>(), &end);
    return end != v.as<const char *>() ? f : def;
  }
  return v.as<float>();
}

// Тривалість: durationMs (мс) або duration (с, як в AWTRIX 3)
static uint32_t parse_ms(JsonObjectConst o, const char *ms_key, const char *sec_key) {
  if (!o[ms_key].isNull())
    return (uint32_t) std::max(0.0f, num(o[ms_key], 0));
  if (!o[sec_key].isNull())
    return (uint32_t) std::max(0.0f, num(o[sec_key], 0) * 1000.0f);
  return 0;
}

// ---------------------------------------------------------------------------
// Реєстрація PUT / PATCH / DELETE
// ---------------------------------------------------------------------------
void MatrixWeb::register_raw_methods_() {
  auto *server = this->base_->get_server();
  if (server == nullptr || server->get_server() == nullptr)
    return;
  const httpd_method_t methods[] = {HTTP_PUT, HTTP_PATCH, HTTP_DELETE};
  for (auto m : methods) {
    httpd_uri_t uri{};
    uri.uri = "";
    uri.method = m;
    uri.handler = &MatrixWeb::raw_handler_;
    uri.user_ctx = this;
    const esp_err_t err = httpd_register_uri_handler(server->get_server(), &uri);
    if (err != ESP_OK && err != ESP_ERR_HTTPD_HANDLER_EXISTS)
      ESP_LOGW(TAG, "Cannot register method %d: %s", (int) m, esp_err_to_name(err));
  }
  this->raw_registered_ = true;
}

esp_err_t MatrixWeb::raw_handler_(httpd_req_t *r) {
  auto *self = static_cast<MatrixWeb *>(r->user_ctx);
  RawRequest req(r);
#ifdef USE_WEBSERVER_AUTH
  if (!self->auth_user_.empty() && !req.authenticate(self->auth_user_.c_str(), self->auth_pass_.c_str())) {
    req.requestAuthentication();
    return ESP_OK;
  }
#endif
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef ref = req.url_to(buf);
  const std::string url(ref.c_str(), ref.size());
  if (url.rfind("/api/v1/", 0) != 0) {
    req.send(404, "application/json", error_json("notFound", "unknown route").c_str());
    return ESP_OK;
  }
  const size_t limit = url.rfind("/api/v1/icons/", 0) == 0 ? ICON_MAX_BYTES : MAX_JSON_BODY;
  if (r->content_len > limit) {
    req.send(413, "application/json", error_json("tooLarge", "request body is too large").c_str());
    return ESP_OK;
  }
  std::string body;
  body.resize(r->content_len);
  size_t got = 0;
  while (got < r->content_len) {
    const int n = httpd_req_recv(r, &body[got], r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (n <= 0)
      return ESP_FAIL;
    got += n;
  }
  const Reply rep = self->handle_v1_(r->method, url, body);
  req.send(rep.code, rep.type.c_str(), rep.body.c_str());
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// JSON-відповіді
// ---------------------------------------------------------------------------
std::string MatrixWeb::build_apps_json_() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  std::string out = "[";
  for (size_t i = 0; i < this->order_.size(); i++) {
    const std::string &name = this->order_[i];
    const bool builtin = this->is_builtin_(name);
    const bool en = this->app_enabled_(name);
    if (i)
      out += ',';
    out += "{\"name\":";
    jstr(out, name);
    out += ",\"enabled\":";
    out += en ? "true" : "false";
    out += ",\"inLoop\":";
    out += en ? "true" : "false";
    out += ",\"slot\":" + std::to_string(i);
    out += ",\"present\":";
    out += this->app_present_(name) ? "true" : "false";
    const Informer *inf = nullptr;
    for (const auto &x : this->informers_)
      if (x.id == name)
        inf = &x;
    out += ",\"origin\":";
    out += builtin ? "\"builtin\"" : (inf ? "\"informer\"" : "\"pushed\"");
    if (inf) {
      out += ",\"title\":";
      jstr(out, inf->title);
    }
    out += ",\"active\":";
    out += (int) i == this->current_ && this->notes_.empty() ? "true" : "false";
    out += ",\"durationMs\":" + std::to_string(this->app_duration_ms_(name));
    if (const auto *a = this->find_pushed_(name)) {
      char col[8];
      out += ",\"text\":";
      jstr(out, a->text);
      out += ",\"icon\":";
      jstr(out, a->icon);
      snprintf(col, sizeof(col), "#%06X", (unsigned) a->color);
      out += ",\"color\":\"";
      out += col;
      out += "\",\"progress\":" + std::to_string(a->progress);
      out += ",\"lifetimeMs\":" + std::to_string(a->lifetime_ms);
    }
    out += '}';
  }
  out += ']';
  return out;
}

std::string MatrixWeb::build_icons_json_() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  std::string out = "{\"icons\":[";
  for (size_t i = 0; i < this->icons_.size(); i++) {
    const auto &ic = *this->icons_[i];
    if (i)
      out += ',';
    out += "{\"name\":";
    jstr(out, ic.name);
    out += ",\"width\":" + std::to_string(ic.w);
    out += ",\"height\":" + std::to_string(ic.h);
    out += ",\"frames\":" + std::to_string(ic.frames());
    out += ",\"bytes\":" + std::to_string(ICON_HEADER + ic.frames() * (2 + ic.frame_bytes()));
    out += '}';
  }
  out += "],\"usedBytes\":" + std::to_string(this->icons_bytes_());
  out += ",\"totalBytes\":" + std::to_string(ICON_BUDGET);
  out += ",\"maxIcons\":" + std::to_string(ICON_MAX_COUNT) + "}";
  return out;
}

// Кадр у кольорах «як бачить око»: лінійні значення стрічки → назад через гамму 2.8
std::string MatrixWeb::build_screen_json_() const {
  static uint8_t inv[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; i++)
      inv[i] = (uint8_t) (powf(i / 255.0f, 1.0f / 2.8f) * 255.0f + 0.5f);
    ready = true;
  }
  std::string out;
  out.reserve(this->width_ * this->height_ * 9 + 64);
  out += "{\"width\":" + std::to_string(this->width_);
  out += ",\"height\":" + std::to_string(this->height_);
  out += ",\"pixels\":[";
  for (int y = 0; y < this->height_; y++)
    for (int x = 0; x < this->width_; x++) {
      Color c(0, 0, 0);
      this->read_pixel_(x, y, c);
      const uint32_t v = (inv[c.r] << 16) | (inv[c.g] << 8) | inv[c.b];
      if (x || y)
        out += ',';
      out += std::to_string(v);
    }
  out += "]}";
  return out;
}

// ---------------------------------------------------------------------------
// Маршрутизація /api/v1
// ---------------------------------------------------------------------------
MatrixWeb::Reply MatrixWeb::handle_v1_(int method, const std::string &url_in, const std::string &body) {
  Reply rep;
  const std::string url = url_in.size() > 8 && url_in.back() == '/' ? url_in.substr(0, url_in.size() - 1) : url_in;
  const std::string path = url.substr(strlen("/api/v1/"));
  auto err = [&](int code, const char *c, const std::string &msg) {
    rep.code = code;
    rep.body = error_json(c, msg);
    return rep;
  };
  auto ok = [&]() {
    rep.body = "{\"ok\":true}";
    return rep;
  };
  auto parse = [&](JsonDocument &doc) -> bool {
    if (body.empty())
      return false;
    doc = json::parse_json(body);
    return doc.is<JsonObject>();
  };
  const bool is_get = method == HTTP_GET, is_post = method == HTTP_POST, is_put = method == HTTP_PUT,
             is_patch = method == HTTP_PATCH, is_del = method == HTTP_DELETE;

  // ----- apps -----
  if (path == "apps") {
    if (!is_get)
      return err(405, "methodNotAllowed", "allowed: GET");
    rep.body = this->build_apps_json_();
    return rep;
  }
  if (path == "apps/next" || path == "apps/previous") {
    if (!(is_post || is_put))
      return err(405, "methodNotAllowed", "allowed: POST, PUT");
    const bool next = path == "apps/next";
    this->defer([this, next]() { next ? this->next_app() : this->prev_app(); });
    return ok();
  }
  if (path == "apps/active") {
    if (!(is_put || is_post))
      return err(405, "methodNotAllowed", "allowed: PUT");
    JsonDocument doc;
    if (!parse(doc))
      return err(400, "badRequest", "JSON body {\"name\": ...} expected");
    const std::string name = doc["name"] | "";
    {
      std::lock_guard<std::recursive_mutex> guard(this->lock_);
      if (!this->app_present_(name))
        return err(404, "notFound", "no such app");
    }
    // interrupt: true — показати й потім повернутися до перерваного; інакше — перейти до нього
    const bool interrupt = doc["interrupt"] | false;
    this->defer([this, name, interrupt]() { interrupt ? (void) this->interrupt_with(name) : (void) this->switch_to(name); });
    return ok();
  }
  if (path == "apps/order") {
    if (!(is_put || is_post))
      return err(405, "methodNotAllowed", "allowed: PUT");
    JsonDocument doc;
    if (!parse(doc))
      return err(400, "badRequest", "JSON body {\"order\": [...], \"disabled\": [...]} expected");
    std::vector<std::string> order, disabled;
    for (JsonVariantConst v : doc["order"].as<JsonArrayConst>())
      order.push_back(v.as<std::string>());
    for (JsonVariantConst v : doc["disabled"].as<JsonArrayConst>())
      disabled.push_back(v.as<std::string>());
    this->defer([this, order, disabled]() { this->apply_order_(order, disabled); });
    return ok();
  }
  if (path.rfind("apps/", 0) == 0) {
    const std::string name = url_decode(path.substr(5));
    if (name.find('/') != std::string::npos)
      return err(404, "notFound", "unknown route");
    if (is_del) {
      if (this->is_builtin_(name))
        return err(400, "badRequest", "built-in apps cannot be deleted");
      if (name.rfind("inf_", 0) == 0) {
        this->defer([this, name]() { this->remove_informer_(name); });
        return ok();
      }
      this->defer([this, name]() { this->remove_pushed_(name); });
      return ok();
    }
    if (is_post || is_put) {
      // Розширення: створити / оновити pushed-застосунок
      if (!valid_name(name) || this->is_builtin_(name) || name.rfind("inf_", 0) == 0)
        return err(400, "badRequest", "app name: 1-32 chars of A-Z a-z 0-9 _ -, not a built-in, not inf_*");
      JsonDocument doc;
      if (!parse(doc))
        return err(400, "badRequest", "JSON body expected");
      JsonObjectConst o = doc.as<JsonObjectConst>();
      PushedApp a;
      a.name = name;
      a.text = o["text"] | "";
      a.icon = o["icon"] | "";
      a.color = parse_color(o["color"], 0xFFFFFF);
      a.duration_ms = parse_ms(o, "durationMs", "duration");
      a.lifetime_ms = parse_ms(o, "lifetimeMs", "lifetime");
      a.progress = std::max(-1, std::min(100, (int) lroundf(num(o["progress"], -1))));
      a.progress_color = parse_color(o["progressColor"].isNull() ? o["progressC"] : o["progressColor"], 0x00A0FF);
      const bool show = o["show"] | false;
      const bool interrupt = o["interrupt"] | false;  // показати зараз і повернутися до ротації
      this->defer([this, a, show, interrupt]() mutable {
        const std::string n = a.name;
        this->upsert_pushed_(std::move(a));
        if (interrupt)
          this->interrupt_with(n);
        else if (show)
          this->switch_to(n);
      });
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: POST, PUT, DELETE");
  }

  // ----- informers (застосунки користувача) -----
  if (path == "informers") {
    if (is_get) {
      rep.body = this->informers_json_();
      return rep;
    }
    if (is_post) {
      // Новий інформер із шаблону (або з тіла запиту)
      Informer inf;
      {
        std::lock_guard<std::recursive_mutex> guard(this->lock_);
        if ((int) this->informers_.size() >= INFORMER_MAX)
          return err(507, "storageFull", "too many informers");
        inf.id = "inf_" + std::to_string(this->informer_seq_++);
      }
      JsonDocument doc;
      if (parse(doc) && !doc["lines"].isNull()) {
        parse_informer(doc.as<JsonObjectConst>(), inf);
      } else {
        inf.title = "Інформер " + inf.id.substr(4);
        inf.line[0].fmt = "Текст";
        inf.line[0].color = 0xFFD200;
        inf.line[1].color = 0xFFFFFF;
      }
      if (inf.title.empty())
        inf.title = "Інформер " + inf.id.substr(4);
      this->defer([this, inf]() { this->upsert_informer_(inf); });
      rep.body = "{\"ok\":true,\"id\":\"" + inf.id + "\"}";
      return rep;
    }
    return err(405, "methodNotAllowed", "allowed: GET, POST");
  }
  if (path.rfind("informers/", 0) == 0) {
    const std::string id = url_decode(path.substr(10));
    {
      std::lock_guard<std::recursive_mutex> guard(this->lock_);
      if (this->find_informer_(id) == nullptr)
        return err(404, "notFound", "no such informer");
    }
    if (is_del) {
      this->defer([this, id]() { this->remove_informer_(id); });
      return ok();
    }
    if (is_put || is_post || is_patch) {
      JsonDocument doc;
      if (!parse(doc))
        return err(400, "badRequest", "JSON body expected");
      Informer inf;
      inf.id = id;
      parse_informer(doc.as<JsonObjectConst>(), inf);
      if (inf.title.empty())
        inf.title = "Інформер " + id.substr(4);
      this->defer([this, inf]() { this->upsert_informer_(inf); });
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: PUT, DELETE");
  }

  // ----- notifications -----
  if (path == "notifications") {
    if (!is_post)
      return err(405, "methodNotAllowed", "allowed: POST");
    JsonDocument doc;
    if (!parse(doc))
      return err(400, "badRequest", "JSON body expected");
    JsonObjectConst o = doc.as<JsonObjectConst>();
    Notification n;
    n.name = o["name"] | "";
    n.text = o["text"] | "";
    n.icon = o["icon"] | "";
    n.color = parse_color(o["color"], 0xFFFFFF);
    // Рамка: явно заданий frameColor; без іконки — колір рамки з налаштувань
    if (!o["frameColor"].isNull())
      n.frame_color = parse_color(o["frameColor"], 0);
    n.duration_ms = parse_ms(o, "durationMs", "duration");
    n.hold = o["hold"] | false;
    const bool stack = o["stack"] | true;
    if (n.text.empty() && n.icon.empty())
      return err(400, "badRequest", "text or icon required");
    this->defer([this, n, stack]() mutable { this->notify(std::move(n), stack); });
    return ok();
  }
  if (path.rfind("notifications/", 0) == 0) {
    if (!is_del)
      return err(405, "methodNotAllowed", "allowed: DELETE");
    const std::string name = url_decode(path.substr(14));
    this->defer([this, name]() { this->dismiss(name == "active" ? "" : name); });
    return ok();
  }

  // ----- display -----
  if (path == "display") {
    if (is_get) {
      const float pct = this->number_("brightness_pct", 50);
      rep.body = "{\"power\":";
      rep.body += (this->light_ != nullptr && this->light_->remote_values.is_on()) ? "true" : "false";
      rep.body += ",\"brightness\":" + std::to_string((int) lroundf(pct * 2.55f));
      rep.body += ",\"brightnessPct\":" + std::to_string((int) lroundf(pct));
      rep.body += ",\"autoBrightness\":";
      rep.body += this->switch_("auto_brightness", false) ? "true" : "false";
      rep.body += "}";
      return rep;
    }
    if (is_patch || is_put || is_post) {
      JsonDocument doc;
      if (!parse(doc))
        return err(400, "badRequest", "JSON body expected");
      JsonObjectConst o = doc.as<JsonObjectConst>();
      if (!o["power"].isNull()) {
        const bool on = o["power"].as<bool>();
        this->defer([this, on]() { this->apply_set_("light", "", on ? "on" : "off"); });
      }
      float pct = NAN;
      if (!o["brightnessPct"].isNull())
        pct = o["brightnessPct"].as<float>();
      else if (!o["brightness"].isNull())
        pct = o["brightness"].as<float>() / 2.55f;
      if (!std::isnan(pct)) {
        const std::string v = std::to_string((int) lroundf(std::max(5.0f, std::min(100.0f, pct))));
        this->defer([this, v]() { this->apply_set_("number", "brightness_pct", v); });
      }
      if (!o["autoBrightness"].isNull()) {
        const bool on = o["autoBrightness"].as<bool>();
        this->defer([this, on]() { this->apply_set_("switch", "auto_brightness", on ? "on" : "off"); });
      }
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: GET, PATCH");
  }
  if (path == "display/screen") {
    if (!is_get)
      return err(405, "methodNotAllowed", "allowed: GET");
    rep.body = this->build_screen_json_();
    return rep;
  }

  // ----- hardware: підключення матриці (застосовується після перезавантаження) -----
  if (path == "hardware") {
    auto *hub = StripHub::get();
    auto cfg_json = [](const char *key, const StripConfig &c) {
      std::string o = std::string("\"") + key + "\":{\"mode\":\"";
      o += c.mode == (uint8_t) StripMode::CHAIN ? "chain" : "direct";
      o += "\",\"pin\":" + std::to_string(c.pin);
      o += ",\"cols\":" + std::to_string(c.cols);
      o += ",\"chain\":\"";
      o += c.chain == (uint8_t) ChainOrder::SNAKE ? "snake" : "rows";
      o += "\",\"serpentine\":";
      o += c.serpentine ? "true" : "false";
      o += '}';
      return o;
    };
    if (is_get) {
      StripConfig active = hub->saved();
      active.mode = (uint8_t) hub->mode();
      active.pin = hub->mode() == StripMode::DIRECT ? hub->pin() : hub->saved().pin;
      active.cols = hub->cols();
      active.chain = (uint8_t) hub->chain();
      active.serpentine = hub->serpentine();
      rep.body = "{" + cfg_json("active", active) + "," + cfg_json("saved", hub->saved());
      rep.body += ",\"statusPin\":" + std::to_string(hub->status_pin());
      rep.body += ",\"width\":" + std::to_string(hub->width());
      rep.body += ",\"height\":" + std::to_string(hub->height());
      rep.body += ",\"colsMin\":" + std::to_string(COLS_MIN) + ",\"colsMax\":" + std::to_string(COLS_MAX);
      rep.body += ",\"colsDefault\":" + std::to_string(hub->default_cols());
      rep.body += ",\"pins\":[";
      bool first = true;
      for (uint8_t p : hub->allowed_pins()) {
        if (!first)
          rep.body += ',';
        first = false;
        rep.body += std::to_string(p);
      }
      rep.body += "]}";
      return rep;
    }
    if (is_put || is_post || is_patch) {
      JsonDocument doc;
      if (!parse(doc))
        return err(400, "badRequest", "JSON body {mode, pin, cols, chain, serpentine, reboot} expected");
      StripConfig c = hub->saved();
      const std::string mode = doc["mode"] | (c.mode ? "direct" : "chain");
      if (mode != "chain" && mode != "direct")
        return err(400, "badRequest", "mode: chain або direct");
      c.mode = mode == "direct" ? 1 : 0;
      const std::string chain = doc["chain"] | (c.chain ? "snake" : "rows");
      if (chain != "rows" && chain != "snake")
        return err(400, "badRequest", "chain: rows або snake");
      c.chain = chain == "snake" ? 1 : 0;
      if (!doc["pin"].isNull())
        c.pin = (uint8_t) num(doc["pin"], c.pin);
      if (!doc["cols"].isNull())
        c.cols = (uint8_t) num(doc["cols"], c.cols);
      if (!doc["serpentine"].isNull())
        c.serpentine = doc["serpentine"].as<bool>();
      std::string why;
      if (!hub->save(c, why))
        return err(400, "badRequest", why);
      if (doc["reboot"] | false)
        this->set_timeout(800, []() { App.safe_reboot(); });
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: GET, PUT");
  }

  // ----- wifi (налаштування мережі з веб-панелі) -----
  if (path == "wifi") {
#ifdef USE_WIFI
    auto *w = wifi::global_wifi_component;
    if (is_get) {
      std::string out = "{\"connected\":";
      out += w->is_connected() ? "true" : "false";
      char ssid[wifi::SSID_BUFFER_SIZE] = {0};
      out += ",\"ssid\":";
      jstr(out, w->is_connected() ? std::string(w->wifi_ssid_to(ssid)) : std::string());
      out += ",\"rssi\":" + std::to_string(w->is_connected() ? w->wifi_rssi() : 0);
      out += ",\"ip\":\"";
      char ip[network::IP_ADDRESS_BUFFER_SIZE] = {0};
      for (auto &a : w->wifi_sta_ip_addresses())
        if (a.is_set()) {
          a.str_to(ip);
          break;
        }
      out += ip;
      out += "\",\"apActive\":";
      out += w->is_ap_active() ? "true" : "false";
      out += ",\"networks\":[";
      {
        wifi::ScanResultsLock lock(w);
        const auto &results = w->get_scan_result();
        bool first = true;
        for (const auto &scan : results) {
          bool with_auth = false;
          if (!wifi::should_show_scan_entry(results, scan, with_auth))
            continue;
          if (!first)
            out += ',';
          first = false;
          out += "{\"ssid\":";
          const StringRef s = scan.get_ssid();
          jstr(out, std::string(s.c_str(), s.size()));
          out += ",\"rssi\":" + std::to_string(scan.get_rssi());
          out += ",\"lock\":";
          out += with_auth ? "true" : "false";
          out += '}';
        }
      }
      out += "]}";
      rep.body = out;
      return rep;
    }
    if (is_post || is_put) {
      JsonDocument doc;
      if (!parse(doc))
        return err(400, "badRequest", "JSON body {\"ssid\": ..., \"password\": ...} expected");
      const std::string ssid = doc["ssid"] | "";
      const std::string pass = doc["password"] | "";
      if (ssid.empty() || ssid.size() > 32)
        return err(400, "badRequest", "ssid: 1-32 chars");
      if (!pass.empty() && (pass.size() < 8 || pass.size() > 64))
        return err(400, "badRequest", "password: empty or 8-64 chars");
      ESP_LOGI(TAG, "Wi-Fi settings change requested: SSID='%s'", ssid.c_str());
      // Невелика затримка, щоб відповідь встигла піти до перемикання мережі
      this->set_timeout(500, [ssid, pass]() { wifi::global_wifi_component->save_wifi_sta(ssid, pass); });
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: GET, POST");
#else
    return err(404, "notFound", "built without Wi-Fi");
#endif
  }

  // ----- icons -----
  if (path == "icons") {
    if (!is_get)
      return err(405, "methodNotAllowed", "allowed: GET");
    rep.body = this->build_icons_json_();
    return rep;
  }
  if (path.rfind("icons/", 0) == 0) {
    const std::string name = url_decode(path.substr(6));
    if (!valid_name(name, ICON_NAME_MAX))
      return err(400, "badRequest", "icon name: 1-23 chars of A-Z a-z 0-9 _ -");
    if (is_get) {
      std::lock_guard<std::recursive_mutex> guard(this->lock_);
      const Icon *ic = this->icon(name);
      if (ic == nullptr)
        return err(404, "notFound", "no such icon");
      rep.type = "application/octet-stream";
      rep.body = ic->encode();
      return rep;
    }
    if (is_del) {
      this->defer([this, name]() { this->remove_icon_(name); });
      return ok();
    }
    if (is_put || is_post) {
      auto ic = std::make_shared<Icon>();
      std::string why;
      if (!Icon::decode(reinterpret_cast<const uint8_t *>(body.data()), body.size(), *ic, why))
        return err(400, "badRequest", "icon: " + why);
      ic->name = name;
      {
        std::lock_guard<std::recursive_mutex> guard(this->lock_);
        const Icon *old = this->icon(name);
        const size_t old_bytes = old ? ICON_HEADER + old->frames() * (2 + old->frame_bytes()) : 0;
        if (old == nullptr && (int) this->icons_.size() >= ICON_MAX_COUNT)
          return err(507, "storageFull", "too many icons");
        if (this->icons_bytes_() - old_bytes + body.size() > ICON_BUDGET)
          return err(507, "storageFull", "icon storage is full");
      }
      this->defer([this, ic]() { this->put_icon_(ic); });
      return ok();
    }
    return err(405, "methodNotAllowed", "allowed: GET, PUT, POST, DELETE");
  }

  return err(404, "notFound", "unknown route");
}

}  // namespace esphome::matrix_web
