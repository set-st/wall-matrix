#pragma once
// Стрічка WS2812 з підключенням, що задається з веб-панелі (режим і пін читаються з флешу
// під час завантаження; зміна — через перезавантаження).
//
//  CHAIN  — матриця підключена ПІСЛЯ вбудованого світлодіода плати: одна стрічка на
//           status_pin, піксель 0 — індикатор, далі — матриця (вбудований WS2812 піднімає
//           рівень сигналу до ~4.7 В).
//  DIRECT — матриця на окремому піні, вбудований світлодіод — окремий індикатор на status_pin.
//
// Дві логічні стрічки (роль matrix і status) — звичайні AddressableLight для ESPHome.

#include <cstdint>
#include <string>
#include <vector>

#include <driver/rmt_tx.h>

#include "esphome/components/light/addressable_light.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::matrix_web {

enum class StripMode : uint8_t { CHAIN = 0, DIRECT = 1 };

// Порядок модулів 8×8 у ланцюжку: ROWS — кожен ряд зліва направо (у кінці ряду провід
// повертається до початку наступного); SNAKE — другий ряд іде справа наліво, модулі в ньому
// перевернуті на 180° (короткі дроти між рядами).
enum class ChainOrder : uint8_t { ROWS = 0, SNAKE = 1 };

static constexpr int PANEL = 8;      // модуль 8×8
static constexpr int PANEL_ROWS = 2;  // висота — 2 модулі (16 px)
static constexpr int COLS_MIN = 2, COLS_MAX = 8;  // ширина 16…64 px

struct StripConfig {
  uint8_t mode{0};        // StripMode
  uint8_t pin{5};         // пін матриці в режимі DIRECT
  uint8_t serpentine{0};  // «змійка» всередині панелі 8×8
  uint8_t version{2};
  uint8_t cols{4};        // ширина в модулях
  uint8_t chain{0};       // ChainOrder
  uint8_t reserved[2]{};
};

struct StripConfigV1 {
  uint8_t mode, pin, serpentine, version;
};

class StripHub {
 public:
  static StripHub *get();

  void set_defaults(StripMode mode, uint8_t pin, uint8_t status_pin, uint8_t cols, ChainOrder chain, bool serpentine);
  void add_reserved_pin(uint8_t pin) { this->reserved_.push_back(pin); }
  void init();  // повторні виклики нічого не роблять

  // Поточна (активна) конфігурація
  StripMode mode() const { return (StripMode) this->active_.mode; }
  uint8_t pin() const { return this->mode() == StripMode::CHAIN ? this->status_pin_ : this->active_.pin; }
  uint8_t status_pin() const { return this->status_pin_; }
  bool serpentine() const { return this->active_.serpentine; }
  int cols() const { return this->active_.cols; }
  int default_cols() const { return this->defaults_.cols; }  // із YAML (matrix_cols)
  ChainOrder chain() const { return (ChainOrder) this->active_.chain; }
  int width() const { return this->active_.cols * PANEL; }
  int height() const { return PANEL_ROWS * PANEL; }
  uint16_t matrix_leds() const { return this->width() * this->height(); }
  // Індекс світлодіода в ланцюжку матриці для пікселя (x, y); -1 — поза матрицею
  int index(int x, int y) const;
  // Збережена — набуде чинності після перезавантаження
  const StripConfig &saved() const { return this->saved_; }
  bool save(const StripConfig &c, std::string &err);
  std::vector<uint8_t> allowed_pins() const;
  bool pin_allowed(uint8_t pin) const;

  uint8_t *pixels(int role);  // 0 — матриця, 1 — індикатор (GRB, по 3 байти)
  void show(int role);

 protected:
  struct Chan {
    rmt_channel_handle_t ch{nullptr};
    rmt_encoder_handle_t enc{nullptr};
    uint8_t *buf{nullptr};  // пікселі (пише ESPHome)
    uint8_t *tx{nullptr};   // копія для передачі (поки йде передача, buf можна змінювати)
    size_t len{0};
    uint32_t last_us{0};
    bool ok{false};
  };
  bool open_(Chan &c, uint8_t pin, size_t bytes, size_t symbols);
  void transmit_(Chan &c);

  bool initialized_{false};
  StripConfig defaults_;
  StripConfig active_;
  StripConfig saved_;
  uint8_t status_pin_{48};
  std::vector<uint8_t> reserved_;
  ESPPreferenceObject pref_;
  Chan chans_[2];  // CHAIN: лише [0]; DIRECT: [0] — матриця, [1] — індикатор
  uint8_t *matrix_{nullptr};
  uint8_t *status_{nullptr};
};

class StripLight : public light::AddressableLight {
 public:
  void set_role(int role) { this->role_ = role; }
  void setup() override;
  void write_state(light::LightState *state) override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  int32_t size() const override;
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  void clear_effect_data() override {
    for (auto &e : this->effect_data_)
      e = 0;
  }
  void dump_config() override;

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;
  int role_{0};
  std::vector<uint8_t> effect_data_;
};

}  // namespace esphome::matrix_web
