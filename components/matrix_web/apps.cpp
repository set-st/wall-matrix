// Застосунки (вбудовані + pushed), їх порядок і ротація; черга сповіщень.
#include "matrix_web.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.apps";
static constexpr uint32_t ORDER_HASH = 0x57A11C0Du;
static constexpr int MAX_PUSHED = 16;
static constexpr float SCROLL_PAUSE = 20.0f;  // ≈ 1 с паузи перед прокручуванням

struct OrderStore {
  char data[512];  // імена через '\n'
};

struct Builtin {
  const char *name;
  AppKind kind;
  const char *enable_switch;
  const char *duration_number;
};
static const Builtin BUILTINS[] = {
    {"Time", AppKind::CLOCK, "app_clock", "clock_secs"},
    {"Date", AppKind::DATE, "app_date", "date_secs"},
    {"Weather", AppKind::WEATHER, "app_weather", "weather_secs"},
    {"Text", AppKind::TEXT, "app_text", "text_secs"},
};

static const Builtin *builtin(const std::string &name) {
  for (const auto &b : BUILTINS)
    if (name == b.name)
      return &b;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Допоміжне
// ---------------------------------------------------------------------------
float MatrixWeb::number_(const char *key, float def) const {
  for (const auto &e : this->numbers_)
    if (e.key == key)
      return std::isnan(e.obj->state) ? def : e.obj->state;
  return def;
}

bool MatrixWeb::switch_(const char *key, bool def) const {
  for (const auto &e : this->switches_)
    if (e.key == key)
      return e.obj->state;
  return def;
}

bool MatrixWeb::is_builtin_(const std::string &name) const { return builtin(name) != nullptr; }

PushedApp *MatrixWeb::find_pushed_(const std::string &name) {
  for (auto &a : this->pushed_)
    if (a.name == name)
      return &a;
  return nullptr;
}

bool MatrixWeb::app_present_(const std::string &name) const {
  if (this->is_builtin_(name))
    return true;
  for (const auto &i : this->informers_)
    if (i.id == name)
      return true;
  for (const auto &a : this->pushed_)
    if (a.name == name)
      return true;
  return false;
}

bool MatrixWeb::app_enabled_(const std::string &name) const {
  if (const auto *b = builtin(name))
    return this->switch_(b->enable_switch, true);
  return std::find(this->disabled_.begin(), this->disabled_.end(), name) == this->disabled_.end();
}

// Чи можна показати застосунок прямо зараз
bool MatrixWeb::app_runnable_(const std::string &name) const {
  if (!this->app_present_(name) || !this->app_enabled_(name))
    return false;
  if (name == "Weather") {
    for (const auto &e : this->sensors_)
      if (e.key == "weather_temp")
        return !std::isnan(e.obj->state);
    return false;
  }
  if (name == "Text") {
    for (const auto &e : this->texts_)
      if ((e.key == "text_line1" || e.key == "text_line2") && !e.obj->state.empty())
        return true;
    return false;
  }
  return true;
}

uint32_t MatrixWeb::app_duration_ms_(const std::string &name) const {
  if (const auto *b = builtin(name))
    return (uint32_t) (this->number_(b->duration_number, 10) * 1000.0f);
  for (const auto &i : this->informers_)
    if (i.id == name && i.duration_ms > 0)
      return i.duration_ms;
  for (const auto &a : this->pushed_)
    if (a.name == name && a.duration_ms > 0)
      return a.duration_ms;
  return (uint32_t) (this->number_("text_secs", 10) * 1000.0f);
}

void MatrixWeb::set_app_enabled_(const std::string &name, bool en) {
  if (const auto *b = builtin(name)) {
    for (const auto &e : this->switches_)
      if (e.key == b->enable_switch && e.obj->state != en)
        en ? e.obj->turn_on() : e.obj->turn_off();
    return;
  }
  auto it = std::find(this->disabled_.begin(), this->disabled_.end(), name);
  if (en && it != this->disabled_.end())
    this->disabled_.erase(it);
  else if (!en && it == this->disabled_.end())
    this->disabled_.push_back(name);
}

// ---------------------------------------------------------------------------
// Порядок
// ---------------------------------------------------------------------------
void MatrixWeb::load_order_() {
  this->order_pref_ = global_preferences->make_preference<OrderStore>(ORDER_HASH, true);
  OrderStore st{};
  this->order_.clear();
  if (this->order_pref_.load(&st)) {
    st.data[sizeof(st.data) - 1] = 0;
    std::string s = st.data, cur;
    for (char c : s) {
      if (c == '\n') {
        if (!cur.empty())
          this->order_.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty())
      this->order_.push_back(cur);
  }
  // Вбудовані застосунки завжди є в порядку
  for (const auto &b : BUILTINS)
    if (std::find(this->order_.begin(), this->order_.end(), b.name) == this->order_.end())
      this->order_.push_back(b.name);
}

void MatrixWeb::save_order_() {
  OrderStore st{};
  std::string s;
  for (const auto &n : this->order_) {
    if (s.size() + n.size() + 1 >= sizeof(st.data))
      break;
    s += n;
    s += '\n';
  }
  strncpy(st.data, s.c_str(), sizeof(st.data) - 1);
  this->order_pref_.save(&st);
}

void MatrixWeb::apply_order_(const std::vector<std::string> &order, const std::vector<std::string> &disabled) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const std::string cur = (this->current_ >= 0 && this->current_ < (int) this->order_.size())
                              ? this->order_[this->current_]
                              : "";
  std::vector<std::string> next;
  for (const auto &n : order)
    if (!n.empty() && std::find(next.begin(), next.end(), n) == next.end())
      next.push_back(n);
  // Невказані застосунки — у кінець, у старому порядку
  for (const auto &n : this->order_)
    if (std::find(next.begin(), next.end(), n) == next.end())
      next.push_back(n);
  this->order_ = next;
  // Увімкнені / вимкнені: усе, що в disabled — вимкнено, решта — увімкнено
  for (const auto &n : this->order_)
    this->set_app_enabled_(n, std::find(disabled.begin(), disabled.end(), n) == disabled.end());
  auto it = std::find(this->order_.begin(), this->order_.end(), cur);
  this->current_ = it == this->order_.end() ? 0 : (int) (it - this->order_.begin());
  this->save_order_();
}

// ---------------------------------------------------------------------------
// Ротація
// ---------------------------------------------------------------------------
void MatrixWeb::start_app_(int index) {
  this->current_ = index;
  this->app_started_ms_ = millis();
  this->scroll_[0] = this->scroll_[1] = SCROLL_PAUSE;
}

void MatrixWeb::next_app() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const int n = this->order_.size();
  for (int i = 1; i <= n; i++) {
    const int idx = (this->current_ + i + n) % n;
    if (this->app_runnable_(this->order_[idx])) {
      this->start_app_(idx);
      return;
    }
  }
  this->start_app_(-1);  // нічого показати — годинник
}

void MatrixWeb::prev_app() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const int n = this->order_.size();
  const int from = this->current_ < 0 ? 0 : this->current_;
  for (int i = 1; i <= n; i++) {
    const int idx = ((from - i) % n + n) % n;
    if (this->app_runnable_(this->order_[idx])) {
      this->start_app_(idx);
      return;
    }
  }
  this->start_app_(-1);
}

bool MatrixWeb::switch_to(const std::string &name) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  auto it = std::find(this->order_.begin(), this->order_.end(), name);
  if (it == this->order_.end() || !this->app_present_(name))
    return false;
  this->start_app_((int) (it - this->order_.begin()));
  // Показ на вимогу перериває звичайні сповіщення (крім hold)
  while (!this->notes_.empty() && !this->notes_.front().hold)
    this->notes_.pop_front();
  return true;
}

AppView MatrixWeb::current_app() const {
  AppView v;
  if (this->current_ < 0 || this->current_ >= (int) this->order_.size())
    return v;
  const std::string &name = this->order_[this->current_];
  if (const auto *b = builtin(name)) {
    v.kind = b->kind;
    return v;
  }
  for (const auto &a : this->pushed_)
    if (a.name == name) {
      v.kind = AppKind::PUSHED;
      v.app = &a;
    }
  for (const auto &i : this->informers_)
    if (i.id == name) {
      v.kind = AppKind::INFORMER;
      v.informer = &i;
    }
  return v;
}

// ---------------------------------------------------------------------------
// Pushed-застосунки
// ---------------------------------------------------------------------------
void MatrixWeb::upsert_pushed_(PushedApp app) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  app.updated_ms = millis();
  if (auto *old = this->find_pushed_(app.name)) {
    *old = std::move(app);
    return;
  }
  if ((int) this->pushed_.size() >= MAX_PUSHED) {
    ESP_LOGW(TAG, "Too many pushed apps, '%s' ignored", app.name.c_str());
    return;
  }
  if (std::find(this->order_.begin(), this->order_.end(), app.name) == this->order_.end()) {
    this->order_.push_back(app.name);
    this->save_order_();
  }
  ESP_LOGI(TAG, "App '%s' added", app.name.c_str());
  this->pushed_.push_back(std::move(app));
}

void MatrixWeb::remove_pushed_(const std::string &name) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const bool was_current =
      this->current_ >= 0 && this->current_ < (int) this->order_.size() && this->order_[this->current_] == name;
  this->pushed_.erase(std::remove_if(this->pushed_.begin(), this->pushed_.end(),
                                     [&](const PushedApp &a) { return a.name == name; }),
                      this->pushed_.end());
  auto it = std::find(this->order_.begin(), this->order_.end(), name);
  if (it != this->order_.end()) {
    const int idx = it - this->order_.begin();
    this->order_.erase(it);
    if (this->current_ > idx)
      this->current_--;
    this->save_order_();
  }
  this->disabled_.erase(std::remove(this->disabled_.begin(), this->disabled_.end(), name), this->disabled_.end());
  if (was_current)
    this->next_app();
  ESP_LOGI(TAG, "App '%s' removed", name.c_str());
}

// ---------------------------------------------------------------------------
// Сповіщення
// ---------------------------------------------------------------------------
const Notification *MatrixWeb::current_notification() const {
  if (this->notes_.empty() || this->notes_.front().started_ms == 0)
    return nullptr;
  return &this->notes_.front();
}

void MatrixWeb::notify(Notification n, bool stack) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  n.started_ms = 0;
  // Однойменне — замінюємо на місці
  if (!n.name.empty())
    for (auto &old : this->notes_)
      if (old.name == n.name) {
        const bool showing = &old == &this->notes_.front() && old.started_ms != 0;
        old = std::move(n);
        if (showing) {
          old.started_ms = millis();
        }
        return;
      }
  if (!stack) {
    // Без черги: прибираємо чергу, нове показуємо одразу
    this->notes_.clear();
  }
  if (this->notes_.size() >= 8)
    this->notes_.pop_back();
  this->notes_.push_back(std::move(n));
}

void MatrixWeb::dismiss(const std::string &name) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  if (name.empty()) {
    if (!this->notes_.empty())
      this->notes_.pop_front();
  } else {
    this->notes_.erase(std::remove_if(this->notes_.begin(), this->notes_.end(),
                                      [&](const Notification &n) { return n.name == name; }),
                       this->notes_.end());
  }
  if (!this->notes_.empty())
    this->notes_.front().started_ms = 0;
}

// ---------------------------------------------------------------------------
// Основний цикл
// ---------------------------------------------------------------------------
void MatrixWeb::loop() {
  if (!this->raw_registered_)
    this->register_raw_methods_();

  const uint32_t now = millis();
  std::lock_guard<std::recursive_mutex> guard(this->lock_);

  // Термін життя pushed-застосунків
  for (size_t i = 0; i < this->pushed_.size();) {
    const auto &a = this->pushed_[i];
    if (a.lifetime_ms > 0 && now - a.updated_ms > a.lifetime_ms) {
      const std::string name = a.name;
      this->remove_pushed_(name);
      continue;
    }
    i++;
  }

  // Сповіщення мають пріоритет над застосунками
  if (!this->notes_.empty()) {
    auto &n = this->notes_.front();
    if (n.started_ms == 0) {
      n.started_ms = now | 1;
      this->scroll_[0] = this->scroll_[1] = SCROLL_PAUSE;
      this->scroll_[2] = n.icon.empty() ? 32 : SCROLL_PAUSE;
    } else if (!n.hold) {
      const uint32_t dur = n.duration_ms > 0 ? n.duration_ms
                                             : (uint32_t) (this->number_("notify_duration", 10) * 1000.0f);
      if (now - n.started_ms >= dur) {
        this->notes_.pop_front();
        if (this->notes_.empty())
          this->app_started_ms_ = now;  // застосунок отримує повний час після сповіщення
      }
    }
    return;
  }

  const bool valid = this->current_ >= 0 && this->current_ < (int) this->order_.size();
  if (!valid || !this->app_runnable_(this->order_[this->current_])) {
    // Поточний став недоступним (або ще нічого не обрано)
    if (now - this->app_started_ms_ >= 1000 || valid)
      this->next_app();
    return;
  }
  if (now - this->app_started_ms_ >= this->app_duration_ms_(this->order_[this->current_]))
    this->next_app();
}

}  // namespace esphome::matrix_web
