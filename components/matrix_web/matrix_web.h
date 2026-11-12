#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

// ---------------------------------------------------------------------------
// Іконки: бібліотека за іменами (8×8 або 16×16, до 16 кадрів), зберігається у флеші.
// Двійковий формат (і для API, і для флешу): 'W' 'M' кадри ширина висота 0,
// далі на кожен кадр: затримка u16 LE (мс) + ширина×висота×3 байти RGB.
// ---------------------------------------------------------------------------
static constexpr int ICON_MAX_FRAMES = 16;
static constexpr int ICON_MAX_COUNT = 32;
static constexpr size_t ICON_NAME_MAX = 23;
static constexpr size_t ICON_BUDGET = 256 * 1024;
static constexpr size_t ICON_HEADER = 6;
static constexpr size_t ICON_MAX_BYTES = ICON_HEADER + ICON_MAX_FRAMES * (2 + 16 * 16 * 3);

struct Icon {
  std::string name;
  uint8_t w{0};
  uint8_t h{0};
  std::vector<uint16_t> delays;
  std::vector<uint8_t> rgb;

  int frames() const { return (int) this->delays.size(); }
  size_t frame_bytes() const { return (size_t) this->w * this->h * 3; }
  const uint8_t *frame_at(uint32_t now_ms) const;
  std::string encode() const;
  static bool decode(const uint8_t *data, size_t len, Icon &out, std::string &err);
};

// ---------------------------------------------------------------------------
// Застосунки та сповіщення (модель як в AWTRIX NG)
// ---------------------------------------------------------------------------
enum class AppKind : uint8_t { CLOCK, DATE, WEATHER, TEXT, PUSHED, INFORMER };

// Застосунок, надісланий через API (origin "pushed")
struct PushedApp {
  std::string name;
  std::string text;          // "\n" — другий рядок
  std::string icon;          // ім'я іконки з бібліотеки
  uint32_t color{0xFFFFFF};
  uint32_t duration_ms{0};   // 0 — як у текстового застосунку
  uint32_t lifetime_ms{0};   // 0 — живе до видалення або перезавантаження
  uint32_t updated_ms{0};
  int progress{-1};          // 0..100, -1 — без смужки
  uint32_t progress_color{0x00A0FF};
};

struct Notification {
  std::string name;
  std::string text;
  std::string icon;
  uint32_t color{0xFFFFFF};
  uint32_t frame_color{0};   // 0 — без рамки; інакше колір рамки
  uint32_t duration_ms{0};   // 0 — типова тривалість
  bool hold{false};          // показувати, доки не приберуть
  uint32_t started_ms{0};    // 0 — ще не показувалося
};

// Інформер — застосунок користувача з веб-панелі: два рядки (або один високий) з даними
// сутностей Home Assistant. Зберігається у флеші.
static constexpr int INFORMER_MAX = 12;

struct InformerLine {
  std::string entity;        // сутність HA (порожньо — лише текст)
  std::string attr;          // атрибут ("" — стан)
  std::string fmt;           // шаблон: {v} {v0} {v1} {v2} + {time} {date} {day} {temp}
  std::string icon;          // ім'я з бібліотеки (рядок 1: 16×16 займає обидва рядки)
  uint32_t color{0xFFFFFF};
  bool unit{true};           // одиниця виміру сутності одразу після значення
};

struct Informer {
  std::string id;            // ім'я в черзі застосунків (inf_N)
  std::string title;         // назва для веб-панелі
  bool single{false};        // один рядок високим шрифтом
  uint32_t duration_ms{0};   // 0 — як у текстового застосунку
  InformerLine line[2];
  std::string preview[2];    // поточний текст рядків — для веб-панелі (оновлюється в loop)
};

struct AppView {
  AppKind kind{AppKind::CLOCK};
  const PushedApp *app{nullptr};
  const Informer *informer{nullptr};
};

template<typename T> struct Entry {
  std::string key;
  T *obj;
};

class MatrixWeb : public Component, public AsyncWebHandler {
 public:
  explicit MatrixWeb(web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  // AsyncWebHandler (GET/POST; PUT/PATCH/DELETE реєструються окремо — див. api_v1.cpp)
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

  // --- Налаштування з codegen ---
  void set_index(const uint8_t *data, size_t len) {
    this->index_ = data;
    this->index_len_ = len;
  }
  void set_auth(const std::string &user, const std::string &pass) {
    this->auth_user_ = user;
    this->auth_pass_ = pass;
  }
  void set_light(light::LightState *light) { this->light_ = light; }
  void set_geometry(int w, int h) {
    this->width_ = w;
    this->height_ = h;
  }
  void set_serpentine(bool s) { this->serpentine_ = s; }
  void set_pixel_offset(int o) { this->pixel_offset_ = o; }

  void add_switch(const std::string &k, switch_::Switch *o) { this->switches_.push_back({k, o}); }
  void add_number(const std::string &k, number::Number *o) { this->numbers_.push_back({k, o}); }
  void add_text(const std::string &k, text::Text *o) { this->texts_.push_back({k, o}); }
  void add_select(const std::string &k, select::Select *o) { this->selects_.push_back({k, o}); }
  void add_button(const std::string &k, button::Button *o) { this->buttons_.push_back({k, o}); }
  void add_sensor(const std::string &k, sensor::Sensor *o) { this->sensors_.push_back({k, o}); }
  void add_text_sensor(const std::string &k, text_sensor::TextSensor *o) { this->text_sensors_.push_back({k, o}); }

  // --- Для display lambda і YAML (основний цикл) ---
  AppView current_app() const;
  const Notification *current_notification() const;
  const Icon *icon(const std::string &name) const;
  float &scroll(int i) { return this->scroll_[i]; }  // 0, 1 — рядки; 2 — сповіщення

  void notify(Notification n, bool stack);
  void dismiss(const std::string &name = "");
  void next_app();
  void prev_app();
  bool switch_to(const std::string &name);

  // Стан сутності Home Assistant (attribute "" — сам стан). Можна викликати повторно
  // з іншою сутністю: старий обробник замінюється (див. ha.cpp).
  using HaCallback = std::function<void(const std::string &)>;
  void watch_ha(const std::string &entity, const std::string &attribute, HaCallback cb);
  const std::string *ha_value(const std::string &entity, const std::string &attribute) const;

  // Текст рядка інформера з підставленим значенням сутності
  std::string informer_text(const Informer &inf, int line) const;

 protected:
  // --- REST ---
  struct Reply {
    int code{200};
    std::string type{"application/json"};
    std::string body;
  };
  Reply handle_v1_(int method, const std::string &url, const std::string &body);
  static esp_err_t raw_handler_(httpd_req_t *r);
  void register_raw_methods_();

  std::string build_state_() const;
  std::string build_live_() const;
  std::string build_screen_json_() const;
  std::string build_apps_json_();
  std::string build_icons_json_();
  bool read_pixel_(int x, int y, Color &c) const;

  void apply_set_(const std::string &type, const std::string &key, const std::string &value);
  void apply_press_(const std::string &key);

  // --- Застосунки ---
  bool is_builtin_(const std::string &name) const;
  bool app_present_(const std::string &name) const;
  bool app_enabled_(const std::string &name) const;
  bool app_runnable_(const std::string &name) const;
  uint32_t app_duration_ms_(const std::string &name) const;
  PushedApp *find_pushed_(const std::string &name);
  void set_app_enabled_(const std::string &name, bool en);
  void upsert_pushed_(PushedApp app);
  void remove_pushed_(const std::string &name);
  void apply_order_(const std::vector<std::string> &order, const std::vector<std::string> &disabled);
  void start_app_(int index);
  void load_order_();
  void save_order_();

  // --- Інформери ---
  Informer *find_informer_(const std::string &id);
  void load_informers_();
  void save_informers_();
  void upsert_informer_(Informer inf);
  void remove_informer_(const std::string &id);
  void watch_informer_(const Informer &inf);
  void update_informer_previews_();
  uint32_t preview_ms_{0};
  std::string informers_json_();

  // --- Іконки ---
  void load_icons_();
  void put_icon_(std::shared_ptr<Icon> icon);
  void remove_icon_(const std::string &name);
  void save_icon_index_();
  size_t icons_bytes_() const;

  float number_(const char *key, float def) const;
  bool switch_(const char *key, bool def) const;

  web_server_base::WebServerBase *base_;
  const uint8_t *index_{nullptr};
  size_t index_len_{0};
  std::string auth_user_;
  std::string auth_pass_;
  bool raw_registered_{false};

  light::LightState *light_{nullptr};
  int width_{32};
  int height_{16};
  bool serpentine_{false};
  int pixel_offset_{0};

  std::vector<Entry<switch_::Switch>> switches_;
  std::vector<Entry<number::Number>> numbers_;
  std::vector<Entry<text::Text>> texts_;
  std::vector<Entry<select::Select>> selects_;
  std::vector<Entry<button::Button>> buttons_;
  std::vector<Entry<sensor::Sensor>> sensors_;
  std::vector<Entry<text_sensor::TextSensor>> text_sensors_;

  // Тіла запитів (httpd обробляє запити по одному)
  std::string body_;
  std::vector<uint8_t> upload_;

  // Стан. Змінюється лише в основному циклі; читання з потоку httpd — під lock_.
  mutable std::recursive_mutex lock_;
  std::vector<std::string> order_;          // порядок (вбудовані + pushed, зокрема відсутні)
  std::vector<std::string> disabled_;       // вимкнені pushed-застосунки
  std::vector<PushedApp> pushed_;
  std::vector<Informer> informers_;
  int informer_seq_{1};
  ESPPreferenceObject informer_pref_;
  std::deque<Notification> notes_;
  std::vector<std::shared_ptr<Icon>> icons_;
  int current_{0};
  uint32_t app_started_ms_{0};
  float scroll_[3]{20, 20, 32};

  struct HaSub {
    bool subscribed{false};
    bool has_value{false};
    std::string value;
    HaCallback cb;
  };
  std::map<std::string, HaSub> ha_subs_;

  ESPPreferenceObject order_pref_;
  ESPPreferenceObject icon_index_pref_;
};

}  // namespace esphome::matrix_web
