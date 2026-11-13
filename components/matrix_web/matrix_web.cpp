#include "matrix_web.h"
#include "strip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web";
static constexpr size_t MAX_BODY = 4096;

// ---------------------------------------------------------------------------
// JSON-помічники
// ---------------------------------------------------------------------------
static void json_str(std::string &out, const char *s, size_t n) {
  out += '"';
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
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
static void json_str(std::string &out, const std::string &s) { json_str(out, s.data(), s.size()); }
static void json_str(std::string &out, const char *s) { json_str(out, s, strlen(s)); }

static void json_num(std::string &out, float v) {
  if (std::isnan(v) || std::isinf(v)) {
    out += "null";
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%g", v);
  out += buf;
}

template<typename T> static T *find(const std::vector<Entry<T>> &list, const std::string &key) {
  for (const auto &e : list)
    if (e.key == key)
      return e.obj;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------
void MatrixWeb::setup() {
  // Розмір матриці — із драйвера стрічки (вже ініціалізований: світло має пріоритет HARDWARE)
  StripHub::get()->init();
  this->width_ = StripHub::get()->width();
  this->height_ = StripHub::get()->height();
  this->load_order_();
  this->load_icons_();
  this->load_informers_();
  this->current_ = -1;
  this->app_started_ms_ = millis();
  this->base_->init();
  this->base_->add_handler(this);
  this->register_raw_methods_();
}

void MatrixWeb::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Matrix web panel:\n"
                "  Port: %u\n"
                "  Page size (gzip): %u bytes\n"
                "  Matrix: %dx%d, offset %d, serpentine %s\n"
                "  Auth: %s\n"
                "  Icons: %u (%u bytes)",
                this->base_->get_port(), (unsigned) this->index_len_, this->width_, this->height_,
                this->pixel_offset_, YESNO(this->serpentine_), YESNO(!this->auth_user_.empty()),
                (unsigned) this->icons_.size(), (unsigned) this->icons_bytes_());
}

// ---------------------------------------------------------------------------
// HTTP (GET/POST)
// ---------------------------------------------------------------------------
static std::string get_url(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return std::string(url.c_str(), url.size());
}

// Режим резервної точки доступу: відповідаємо на будь-який GET своєю сторінкою,
// щоб телефон (перевірки /generate_204, /hotspot-detect.html…) відкрив нашу панель,
// а не сторінку captive_portal ESPHome. DNS-перехоплення лишається за captive_portal.
static bool captive_active() {
#ifdef USE_CAPTIVE_PORTAL
  return captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#else
  return false;
#endif
}

bool MatrixWeb::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = get_url(request);
  if (url == "/" || url == "/index.html" || url.rfind("/api/", 0) == 0)
    return true;
  return captive_active() && request->method() == HTTP_GET;
}

void MatrixWeb::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  const bool icon = get_url(request).rfind("/api/v1/icons/", 0) == 0;
  if (icon) {
    if (index == 0) {
      this->upload_.clear();
      if (total <= ICON_MAX_BYTES)
        this->upload_.reserve(total);
    }
    if (this->upload_.size() + len <= ICON_MAX_BYTES)
      this->upload_.insert(this->upload_.end(), data, data + len);
    return;
  }
  if (index == 0)
    this->body_.clear();
  if (this->body_.size() + len <= MAX_BODY)
    this->body_.append(reinterpret_cast<const char *>(data), len);
}

void MatrixWeb::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = get_url(request);

  const bool page = url == "/" || url == "/index.html" ||
                    (captive_active() && request->method() == HTTP_GET && url.rfind("/api/", 0) != 0);
  if (page) {
    auto *response = request->beginResponse(200, "text/html", this->index_, this->index_len_);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
    return;
  }

  if (url.rfind("/api/v1/", 0) == 0) {
    std::string body;
    if (request->method() == HTTP_POST) {
      if (url.rfind("/api/v1/icons/", 0) == 0) {
        body.assign(this->upload_.begin(), this->upload_.end());
        this->upload_.clear();
        this->upload_.shrink_to_fit();
      } else {
        body.swap(this->body_);
      }
    }
    const Reply rep = this->handle_v1_(request->method(), url, body);
    auto *response = request->beginResponse(rep.code, rep.type.c_str(), rep.body);
    request->send(response);
    return;
  }

  if (url == "/api/state") {
    const std::string json = this->build_state_();
    request->send(200, "application/json", json.c_str());
    return;
  }

  if (url == "/api/live") {
    const std::string live = this->build_live_();
    request->send(200, "text/plain", live.c_str());
    return;
  }

  if (request->method() != HTTP_POST) {
    request->send(405, "text/plain", "Method Not Allowed");
    return;
  }

  if (url == "/api/set") {
    const std::string type = request->arg("type");
    const std::string key = request->arg("id");
    const std::string value = request->arg("v");
    this->defer([this, type, key, value]() { this->apply_set_(type, key, value); });
    request->send(200, "text/plain", "OK");
    return;
  }

  if (url == "/api/press") {
    const std::string key = request->arg("id");
    this->defer([this, key]() { this->apply_press_(key); });
    request->send(200, "text/plain", "OK");
    return;
  }

  request->send(404, "text/plain", "Not Found");
}

// ---------------------------------------------------------------------------
// Дані для веб-панелі
// ---------------------------------------------------------------------------
std::string MatrixWeb::build_state_() const {
  std::string out;
  out.reserve(2048);
  out += "{\"w\":";
  out += std::to_string(this->width_);
  out += ",\"h\":";
  out += std::to_string(this->height_);
  out += ",\"light\":";
  out += (this->light_ != nullptr && this->light_->remote_values.is_on()) ? "true" : "false";

  out += ",\"switch\":{";
  for (size_t i = 0; i < this->switches_.size(); i++) {
    if (i) out += ',';
    json_str(out, this->switches_[i].key);
    out += this->switches_[i].obj->state ? ":true" : ":false";
  }

  out += "},\"number\":{";
  for (size_t i = 0; i < this->numbers_.size(); i++) {
    auto *n = this->numbers_[i].obj;
    if (i) out += ',';
    json_str(out, this->numbers_[i].key);
    out += ":{\"v\":";
    json_num(out, n->state);
    out += ",\"min\":";
    json_num(out, n->traits.get_min_value());
    out += ",\"max\":";
    json_num(out, n->traits.get_max_value());
    out += ",\"step\":";
    json_num(out, n->traits.get_step());
    out += '}';
  }

  out += "},\"text\":{";
  for (size_t i = 0; i < this->texts_.size(); i++) {
    if (i) out += ',';
    json_str(out, this->texts_[i].key);
    out += ':';
    json_str(out, this->texts_[i].obj->state);
  }

  out += "},\"select\":{";
  for (size_t i = 0; i < this->selects_.size(); i++) {
    auto *s = this->selects_[i].obj;
    if (i) out += ',';
    json_str(out, this->selects_[i].key);
    out += ":{\"v\":";
    const StringRef cur = s->current_option();
    json_str(out, cur.c_str(), cur.size());
    out += ",\"o\":[";
    for (size_t j = 0; j < s->size(); j++) {
      if (j) out += ',';
      json_str(out, s->option_at(j));
    }
    out += "]}";
  }

  out += "},\"sensor\":{";
  for (size_t i = 0; i < this->sensors_.size(); i++) {
    if (i) out += ',';
    json_str(out, this->sensors_[i].key);
    out += ':';
    json_num(out, this->sensors_[i].obj->state);
  }

  out += "},\"text_sensor\":{";
  for (size_t i = 0; i < this->text_sensors_.size(); i++) {
    if (i) out += ',';
    json_str(out, this->text_sensors_[i].key);
    out += ':';
    json_str(out, this->text_sensors_[i].obj->state);
  }
  out += "}}";
  return out;
}

// Піксель матриці (лінійне значення стрічки, як його записав дисплей)
bool MatrixWeb::read_pixel_(int x, int y, Color &c) const {
  if (this->light_ == nullptr)
    return false;
  auto *strip = static_cast<light::AddressableLight *>(this->light_->get_output());
  const int idx = StripHub::get()->index(x, y);  // та сама розкладка, що й у pixel_mapper
  if (idx < 0 || idx >= strip->size())
    return false;
  c = (*strip)[idx].get();
  return true;
}

// Кадр: width*height пікселів порядково, кожен — 6 hex-символів RRGGBB (лінійні значення).
std::string MatrixWeb::build_live_() const {
  static const char HEX[] = "0123456789abcdef";
  std::string out;
  out.reserve(this->width_ * this->height_ * 6);
  for (int y = 0; y < this->height_; y++)
    for (int x = 0; x < this->width_; x++) {
      Color c(0, 0, 0);
      this->read_pixel_(x, y, c);
      for (uint8_t v : {c.r, c.g, c.b}) {
        out += HEX[v >> 4];
        out += HEX[v & 0x0F];
      }
    }
  return out;
}

// ---------------------------------------------------------------------------
// Дії (виконуються в основному циклі через defer)
// ---------------------------------------------------------------------------
void MatrixWeb::apply_set_(const std::string &type, const std::string &key, const std::string &value) {
  if (type == "light") {
    if (this->light_ == nullptr)
      return;
    if (value == "toggle")
      this->light_->toggle().perform();
    else if (value == "on")
      this->light_->turn_on().perform();
    else
      this->light_->turn_off().perform();
    return;
  }
  if (type == "switch") {
    if (auto *s = find(this->switches_, key)) {
      if (value == "toggle")
        s->toggle();
      else if (value == "on" || value == "true" || value == "1")
        s->turn_on();
      else
        s->turn_off();
    }
    return;
  }
  if (type == "number") {
    if (auto *n = find(this->numbers_, key))
      n->make_call().set_value(strtof(value.c_str(), nullptr)).perform();
    return;
  }
  if (type == "text") {
    if (auto *t = find(this->texts_, key))
      t->make_call().set_value(value).perform();
    return;
  }
  if (type == "select") {
    if (auto *s = find(this->selects_, key))
      s->make_call().set_option(value).perform();
    return;
  }
  ESP_LOGW(TAG, "Unknown set request: type=%s id=%s", type.c_str(), key.c_str());
}

void MatrixWeb::apply_press_(const std::string &key) {
  if (auto *b = find(this->buttons_, key))
    b->press();
  else
    ESP_LOGW(TAG, "Unknown button: %s", key.c_str());
}

}  // namespace esphome::matrix_web
