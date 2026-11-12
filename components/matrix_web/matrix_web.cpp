#include "matrix_web.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web";
static constexpr size_t MAX_BODY = 1024;
// Формат завантаження: 'W' 'M' кадри ширина висота 0, далі на кадр: затримка u16 LE + RGB
static constexpr size_t ICON_HEADER = 6;
static constexpr size_t ICON_MAX_UPLOAD = ICON_HEADER + ICON_MAX_FRAMES * (2 + ICON_FRAME_BYTES);
static constexpr uint32_t ICON_PREF_HASH = 0x57A11C0Bu;

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
  this->icon_pref_ = global_preferences->make_preference<IconStore>(ICON_PREF_HASH, true);
  if (!this->icon_pref_.load(&this->icon_) || this->icon_.frames > ICON_MAX_FRAMES)
    this->icon_.frames = 0;
  this->base_->init();
  this->base_->add_handler(this);
}

const uint8_t *MatrixWeb::icon_frame(uint32_t now_ms) const {
  const int n = this->icon_.frames;
  if (n == 0)
    return nullptr;
  if (n == 1)
    return this->icon_.data;
  uint32_t total = 0;
  for (int i = 0; i < n; i++)
    total += std::max<uint16_t>(this->icon_.delay_ms[i], 20);
  uint32_t t = now_ms % total;
  for (int i = 0; i < n; i++) {
    const uint32_t d = std::max<uint16_t>(this->icon_.delay_ms[i], 20);
    if (t < d)
      return this->icon_.data + i * ICON_FRAME_BYTES;
    t -= d;
  }
  return this->icon_.data;
}

bool MatrixWeb::commit_icon_upload_() {
  const auto &u = this->upload_;
  if (u.size() < ICON_HEADER || u[0] != 'W' || u[1] != 'M')
    return false;
  const int frames = u[2];
  if (frames < 1 || frames > ICON_MAX_FRAMES || u[3] != ICON_W || u[4] != ICON_H)
    return false;
  if (u.size() != ICON_HEADER + frames * (2 + ICON_FRAME_BYTES))
    return false;
  size_t p = ICON_HEADER;
  for (int i = 0; i < frames; i++) {
    this->icon_.delay_ms[i] = u[p] | (u[p + 1] << 8);
    memcpy(this->icon_.data + i * ICON_FRAME_BYTES, &u[p + 2], ICON_FRAME_BYTES);
    p += 2 + ICON_FRAME_BYTES;
  }
  this->icon_.frames = frames;
  this->save_icon_();
  ESP_LOGI(TAG, "Icon uploaded: %d frame(s)", frames);
  return true;
}

void MatrixWeb::clear_icon_() {
  this->icon_.frames = 0;
  this->save_icon_();
  ESP_LOGI(TAG, "Icon cleared");
}

void MatrixWeb::save_icon_() {
  this->icon_pref_.save(&this->icon_);
  global_preferences->sync();
}

void MatrixWeb::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Matrix web panel:\n"
                "  Port: %u\n"
                "  Page size (gzip): %u bytes\n"
                "  Matrix: %dx%d, offset %d, serpentine %s",
                this->base_->get_port(), (unsigned) this->index_len_, this->width_, this->height_,
                this->pixel_offset_, YESNO(this->serpentine_));
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static std::string get_url(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return std::string(url.c_str(), url.size());
}

bool MatrixWeb::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = get_url(request);
  return url == "/" || url == "/index.html" || url.rfind("/api/", 0) == 0;
}

void MatrixWeb::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (get_url(request) == "/api/icon") {
    if (index == 0) {
      this->upload_.clear();
      if (total <= ICON_MAX_UPLOAD)
        this->upload_.reserve(total);
    }
    if (this->upload_.size() + len <= ICON_MAX_UPLOAD)
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

  if (url == "/" || url == "/index.html") {
    auto *response = request->beginResponse(200, "text/html", this->index_, this->index_len_);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-cache");
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

  if (url == "/api/icon" && request->method() == HTTP_GET) {
    // Той самий формат, що й для завантаження
    std::string out;
    const int n = this->icon_.frames;
    out.reserve(ICON_HEADER + n * (2 + ICON_FRAME_BYTES));
    out += 'W';
    out += 'M';
    out += (char) n;
    out += (char) ICON_W;
    out += (char) ICON_H;
    out += (char) 0;
    for (int i = 0; i < n; i++) {
      out += (char) (this->icon_.delay_ms[i] & 0xFF);
      out += (char) (this->icon_.delay_ms[i] >> 8);
      out.append(reinterpret_cast<const char *>(this->icon_.data + i * ICON_FRAME_BYTES), ICON_FRAME_BYTES);
    }
    auto *response = request->beginResponse(200, "application/octet-stream", out);
    request->send(response);
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

  if (url == "/api/notify") {
    std::string message = this->body_;
    this->body_.clear();
    if (message.empty())
      message = request->arg("text");
    if (message.empty()) {
      request->send(400, "text/plain", "Empty message");
      return;
    }
    this->defer([this, message]() { this->apply_notify_(message); });
    request->send(200, "text/plain", "OK");
    return;
  }

  if (url == "/api/icon") {
    // Застосовуємо в основному циклі, щоб не змінювати іконку посеред кадру
    std::vector<uint8_t> data;
    data.swap(this->upload_);
    const size_t size = data.size();
    if (size < ICON_HEADER || data[0] != 'W' || data[1] != 'M') {
      request->send(400, "text/plain", "Bad icon data");
      return;
    }
    this->defer([this, data = std::move(data)]() mutable {
      this->upload_.swap(data);
      if (!this->commit_icon_upload_())
        ESP_LOGW(TAG, "Icon upload rejected");
      this->upload_.clear();
      this->upload_.shrink_to_fit();
    });
    request->send(200, "text/plain", "OK");
    return;
  }

  if (url == "/api/icon_clear") {
    this->defer([this]() { this->clear_icon_(); });
    request->send(200, "text/plain", "OK");
    return;
  }

  request->send(404, "text/plain", "Not Found");
}

// ---------------------------------------------------------------------------
// Дані
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

  out += "},\"buttons\":[";
  for (size_t i = 0; i < this->buttons_.size(); i++) {
    if (i) out += ',';
    json_str(out, this->buttons_[i].key);
  }
  out += "]}";
  return out;
}

// Кадр: width*height пікселів порядково, кожен — 6 hex-символів RRGGBB.
std::string MatrixWeb::build_live_() const {
  std::string out;
  if (this->light_ == nullptr)
    return out;
  auto *strip = static_cast<light::AddressableLight *>(this->light_->get_output());
  const int32_t n = strip->size();
  static const char HEX[] = "0123456789abcdef";
  out.reserve(this->width_ * this->height_ * 6);
  for (int y = 0; y < this->height_; y++) {
    for (int x = 0; x < this->width_; x++) {
      // Та сама розкладка, що й pixel_mapper: панелі 8×8 зліва направо, згори вниз
      const int tiles_x = this->width_ / 8;
      const int tile = (y / 8) * tiles_x + (x / 8);
      int lx = x % 8;
      const int ly = y % 8;
      if (this->serpentine_ && (ly % 2 == 1))
        lx = 7 - lx;
      const int32_t idx = this->pixel_offset_ + tile * 64 + ly * 8 + lx;
      uint8_t rgb[3] = {0, 0, 0};
      if (idx < n) {
        const Color c = (*strip)[idx].get();
        rgb[0] = c.r;
        rgb[1] = c.g;
        rgb[2] = c.b;
      }
      for (uint8_t v : rgb) {
        out += HEX[v >> 4];
        out += HEX[v & 0x0F];
      }
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

void MatrixWeb::apply_notify_(const std::string &message) {
  if (this->notify_text_ == nullptr || this->notify_button_ == nullptr) {
    ESP_LOGW(TAG, "notify_text / notify_button are not configured");
    return;
  }
  this->notify_text_->make_call().set_value(message).perform();
  this->notify_button_->press();
}

}  // namespace esphome::matrix_web
