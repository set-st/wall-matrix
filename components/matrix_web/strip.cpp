#include "strip.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.strip";
static constexpr uint32_t STRIP_PREF_HASH = 0x57A11C0Fu;
static constexpr uint32_t RMT_HZ = 10 * 1000 * 1000;  // 0.1 мкс на тік
static constexpr uint32_t LATCH_US = 300;             // пауза «скидання» для WS2812 (V5 і новіші)

// Піни ESP32-S3 (N16R8), які безпечно віддати під дані матриці:
// без strapping (0, 3, 45, 46), USB (19, 20), PSRAM (33–37), UART0 (43, 44).
static const uint8_t SAFE_PINS[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47};

StripHub *StripHub::get() {
  static StripHub hub;
  return &hub;
}

void StripHub::set_defaults(StripMode mode, uint8_t pin, uint8_t status_pin, uint16_t matrix_leds, bool serpentine) {
  this->defaults_.mode = (uint8_t) mode;
  this->defaults_.pin = pin;
  this->defaults_.serpentine = serpentine;
  this->status_pin_ = status_pin;
  this->matrix_leds_ = matrix_leds;
}

bool StripHub::pin_allowed(uint8_t pin) const {
  if (pin == this->status_pin_)
    return false;
  if (std::find(this->reserved_.begin(), this->reserved_.end(), pin) != this->reserved_.end())
    return false;
  return std::find(std::begin(SAFE_PINS), std::end(SAFE_PINS), pin) != std::end(SAFE_PINS);
}

std::vector<uint8_t> StripHub::allowed_pins() const {
  std::vector<uint8_t> out;
  for (uint8_t p : SAFE_PINS)
    if (this->pin_allowed(p))
      out.push_back(p);
  return out;
}

bool StripHub::save(const StripConfig &c, std::string &err) {
  if (c.mode > 1) {
    err = "mode: chain або direct";
    return false;
  }
  if (c.mode == (uint8_t) StripMode::DIRECT && !this->pin_allowed(c.pin)) {
    err = "цей пін не можна використати для матриці";
    return false;
  }
  this->saved_ = c;
  this->saved_.version = 1;
  this->pref_.save(&this->saved_);
  global_preferences->sync();
  ESP_LOGI(TAG, "Saved wiring: mode=%s pin=%u serpentine=%u (after reboot)", c.mode ? "direct" : "chain", c.pin,
           c.serpentine);
  return true;
}

bool StripHub::open_(Chan &c, uint8_t pin, size_t bytes, size_t symbols) {
  rmt_tx_channel_config_t cfg{};
  cfg.gpio_num = (gpio_num_t) pin;
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = RMT_HZ;
  cfg.mem_block_symbols = symbols;
  cfg.trans_queue_depth = 1;
  esp_err_t err = rmt_new_tx_channel(&cfg, &c.ch);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RMT channel on GPIO%u: %s", pin, esp_err_to_name(err));
    return false;
  }
  // WS2812: «0» — 0.4 мкс високий + 1.0 мкс низький; «1» — 1.0 + 0.4 мкс
  rmt_bytes_encoder_config_t enc{};
  enc.bit0.level0 = 1;
  enc.bit0.duration0 = 4;
  enc.bit0.level1 = 0;
  enc.bit0.duration1 = 10;
  enc.bit1.level0 = 1;
  enc.bit1.duration0 = 10;
  enc.bit1.level1 = 0;
  enc.bit1.duration1 = 4;
  enc.flags.msb_first = 1;
  if (rmt_new_bytes_encoder(&enc, &c.enc) != ESP_OK || rmt_enable(c.ch) != ESP_OK) {
    ESP_LOGE(TAG, "RMT encoder/enable failed on GPIO%u", pin);
    return false;
  }
  c.len = bytes;
  c.buf = (uint8_t *) heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  c.tx = (uint8_t *) heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  c.ok = c.buf != nullptr && c.tx != nullptr;
  if (!c.ok)
    ESP_LOGE(TAG, "No memory for LED buffer (%u bytes)", (unsigned) bytes);
  return c.ok;
}

void StripHub::init() {
  if (this->initialized_)
    return;
  this->initialized_ = true;

  this->pref_ = global_preferences->make_preference<StripConfig>(STRIP_PREF_HASH, true);
  StripConfig loaded;
  if (this->pref_.load(&loaded) && loaded.version == 1 && loaded.mode <= 1 &&
      (loaded.mode == 0 || this->pin_allowed(loaded.pin))) {
    this->saved_ = loaded;
  } else {
    this->saved_ = this->defaults_;
  }
  this->active_ = this->saved_;

  const size_t mbytes = (size_t) this->matrix_leds_ * 3;
  if (this->mode() == StripMode::CHAIN) {
    // Одна стрічка: [індикатор][матриця]. Уся RMT-пам'ять S3 (192 символи) — їй.
    if (this->open_(this->chans_[0], this->status_pin_, mbytes + 3, 192)) {
      this->status_ = this->chans_[0].buf;
      this->matrix_ = this->chans_[0].buf + 3;
    }
  } else {
    if (this->open_(this->chans_[0], this->active_.pin, mbytes, 144))
      this->matrix_ = this->chans_[0].buf;
    if (this->open_(this->chans_[1], this->status_pin_, 3, 48))
      this->status_ = this->chans_[1].buf;
  }
  ESP_LOGI(TAG, "Wiring: %s, matrix on GPIO%u, status on GPIO%u, serpentine %s",
           this->mode() == StripMode::CHAIN ? "chain (after onboard LED)" : "direct", this->pin(), this->status_pin_,
           YESNO(this->serpentine()));
}

uint8_t *StripHub::pixels(int role) { return role == 0 ? this->matrix_ : this->status_; }

void StripHub::transmit_(Chan &c) {
  if (!c.ok)
    return;
  if (rmt_tx_wait_all_done(c.ch, 100) != ESP_OK) {
    ESP_LOGW(TAG, "RMT busy");
    return;
  }
  // Пауза скидання між кадрами (інакше два кадри зіллються в один)
  const uint32_t since = micros() - c.last_us;
  if (since < LATCH_US)
    delayMicroseconds(LATCH_US - since);
  memcpy(c.tx, c.buf, c.len);
  rmt_transmit_config_t tc{};
  if (rmt_transmit(c.ch, c.enc, c.tx, c.len, &tc) != ESP_OK)
    ESP_LOGW(TAG, "RMT transmit failed");
  // Кадр триває ~30 мкс на світлодіод; рахуємо паузу від його кінця
  c.last_us = micros() + (uint32_t) (c.len / 3) * 30;
}

void StripHub::show(int role) {
  if (this->mode() == StripMode::CHAIN)
    this->transmit_(this->chans_[0]);
  else
    this->transmit_(this->chans_[role == 0 ? 0 : 1]);
}

// ---------------------------------------------------------------------------
// StripLight
// ---------------------------------------------------------------------------
void StripLight::setup() {
  StripHub::get()->init();
  this->effect_data_.assign(this->size(), 0);
}

int32_t StripLight::size() const { return this->role_ == 0 ? StripHub::get()->matrix_leds() : 1; }

void StripLight::write_state(light::LightState *state) {
  this->mark_shown_();
  StripHub::get()->show(this->role_);
}

light::ESPColorView StripLight::get_view_internal(int32_t index) const {
  static uint8_t dummy[4];
  uint8_t *base = StripHub::get()->pixels(this->role_);
  // Порядок байтів у WS2812 — G, R, B
  uint8_t *p = base != nullptr ? base + index * 3 : dummy;
  uint8_t *fx = index < (int32_t) this->effect_data_.size() ? const_cast<uint8_t *>(&this->effect_data_[index]) : dummy + 3;
  return {p + 1, p + 0, p + 2, nullptr, fx, &this->correction_};
}

void StripLight::dump_config() {
  auto *h = StripHub::get();
  ESP_LOGCONFIG(TAG,
                "Matrix strip (%s):\n"
                "  Wiring: %s\n"
                "  Matrix pin: GPIO%u, status pin: GPIO%u, LEDs: %u",
                this->role_ == 0 ? "matrix" : "status", h->mode() == StripMode::CHAIN ? "chain" : "direct", h->pin(),
                h->status_pin(), h->matrix_leds());
}

}  // namespace esphome::matrix_web
