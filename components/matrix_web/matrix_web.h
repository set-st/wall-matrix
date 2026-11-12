#pragma once

#include <string>
#include <vector>

#include "esphome/components/button/button.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::matrix_web {

// Іконка користувача: 16×16, до 16 кадрів (GIF), зберігається у флеші.
static constexpr int ICON_W = 16;
static constexpr int ICON_H = 16;
static constexpr int ICON_MAX_FRAMES = 16;
static constexpr size_t ICON_FRAME_BYTES = ICON_W * ICON_H * 3;

struct IconStore {
  uint8_t frames;  // 0 — іконки немає
  uint8_t reserved[3];
  uint16_t delay_ms[ICON_MAX_FRAMES];
  uint8_t data[ICON_MAX_FRAMES * ICON_FRAME_BYTES];
};

template<typename T> struct Entry {
  std::string key;
  T *obj;
};

class MatrixWeb : public Component, public AsyncWebHandler {
 public:
  explicit MatrixWeb(web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

  // Налаштування з codegen
  void set_index(const uint8_t *data, size_t len) {
    this->index_ = data;
    this->index_len_ = len;
  }
  void set_light(light::LightState *light) { this->light_ = light; }
  void set_geometry(int w, int h) {
    this->width_ = w;
    this->height_ = h;
  }
  void set_serpentine(bool s) { this->serpentine_ = s; }
  void set_pixel_offset(int o) { this->pixel_offset_ = o; }
  void set_notify_text(text::Text *t) { this->notify_text_ = t; }
  void set_notify_button(button::Button *b) { this->notify_button_ = b; }

  void add_switch(const std::string &k, switch_::Switch *o) { this->switches_.push_back({k, o}); }
  void add_number(const std::string &k, number::Number *o) { this->numbers_.push_back({k, o}); }
  void add_text(const std::string &k, text::Text *o) { this->texts_.push_back({k, o}); }
  void add_select(const std::string &k, select::Select *o) { this->selects_.push_back({k, o}); }
  void add_button(const std::string &k, button::Button *o) { this->buttons_.push_back({k, o}); }
  void add_sensor(const std::string &k, sensor::Sensor *o) { this->sensors_.push_back({k, o}); }
  void add_text_sensor(const std::string &k, text_sensor::TextSensor *o) { this->text_sensors_.push_back({k, o}); }

  // Для display lambda: поточний кадр іконки (RGB, 16×16) або nullptr
  const uint8_t *icon_frame(uint32_t now_ms) const;
  bool has_icon() const { return this->icon_.frames > 0; }

 protected:
  std::string build_state_() const;
  std::string build_live_() const;
  void apply_set_(const std::string &type, const std::string &key, const std::string &value);
  void apply_press_(const std::string &key);
  void apply_notify_(const std::string &message);
  bool commit_icon_upload_();
  void clear_icon_();
  void save_icon_();

  web_server_base::WebServerBase *base_;
  const uint8_t *index_{nullptr};
  size_t index_len_{0};

  light::LightState *light_{nullptr};
  int width_{32};
  int height_{16};
  bool serpentine_{false};
  int pixel_offset_{0};

  text::Text *notify_text_{nullptr};
  button::Button *notify_button_{nullptr};

  std::vector<Entry<switch_::Switch>> switches_;
  std::vector<Entry<number::Number>> numbers_;
  std::vector<Entry<text::Text>> texts_;
  std::vector<Entry<select::Select>> selects_;
  std::vector<Entry<button::Button>> buttons_;
  std::vector<Entry<sensor::Sensor>> sensors_;
  std::vector<Entry<text_sensor::TextSensor>> text_sensors_;

  std::string body_;  // тіло поточного POST (httpd обробляє запити по одному)
  std::vector<uint8_t> upload_;  // тіло завантаження іконки

  IconStore icon_{};
  ESPPreferenceObject icon_pref_;
};

}  // namespace esphome::matrix_web
