// Інформери — застосунки користувача: два рядки (або один високий) з даними сутностей HA.
// Зберігаються у флеші як JSON; у черзі застосунків мають origin "informer".
#include "matrix_web.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/components/json/json_util.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.informers";
static constexpr uint32_t INFORMERS_HASH = 0x57A11C0Eu;

struct InformerStore {
  char json[4096];
};

static void jstr(std::string &out, const std::string &s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if ((unsigned char) c >= 0x20)
          out += c;
    }
  }
  out += '"';
}

static std::string color_hex(uint32_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", (unsigned) (c & 0xFFFFFF));
  return buf;
}

// ---------------------------------------------------------------------------
// Серіалізація (той самий формат і для флешу, і для API)
// ---------------------------------------------------------------------------
static void informer_json(std::string &out, const Informer &inf) {
  out += "{\"id\":";
  jstr(out, inf.id);
  out += ",\"title\":";
  jstr(out, inf.title);
  out += ",\"single\":";
  out += inf.single ? "true" : "false";
  out += ",\"durationMs\":" + std::to_string(inf.duration_ms);
  out += ",\"lines\":[";
  for (int i = 0; i < 2; i++) {
    const auto &l = inf.line[i];
    if (i)
      out += ',';
    out += "{\"entity\":";
    jstr(out, l.entity);
    out += ",\"attr\":";
    jstr(out, l.attr);
    out += ",\"fmt\":";
    jstr(out, l.fmt);
    out += ",\"icon\":";
    jstr(out, l.icon);
    out += ",\"color\":\"" + color_hex(l.color) + "\"";
    out += ",\"unit\":";
    out += l.unit ? "true" : "false";
    out += ",\"preview\":";
    jstr(out, inf.preview[i]);
    out += '}';
  }
  out += "]}";
}

static uint32_t parse_hex_color(const char *s, uint32_t def) {
  if (s == nullptr)
    return def;
  if (*s == '#')
    s++;
  if (strlen(s) != 6)
    return def;
  char *end = nullptr;
  const uint32_t v = strtoul(s, &end, 16);
  return (end && *end == 0) ? v : def;
}

static std::string clip(const char *s, size_t max) {
  std::string v = s ? s : "";
  if (v.size() > max)
    v.resize(max);
  return v;
}

// Розбір одного інформера з JSON (id беремо ззовні)
bool parse_informer(JsonObjectConst o, Informer &inf) {
  inf.title = clip(o["title"] | "", 32);
  inf.single = o["single"] | false;
  inf.duration_ms = o["durationMs"].isNull() ? 0 : std::min<uint32_t>(o["durationMs"].as<uint32_t>(), 600000);
  JsonArrayConst lines = o["lines"].as<JsonArrayConst>();
  for (int i = 0; i < 2; i++) {
    JsonObjectConst l = lines.isNull() || (int) lines.size() <= i ? JsonObjectConst() : lines[i].as<JsonObjectConst>();
    auto &d = inf.line[i];
    d.entity = clip(l["entity"] | "", 64);
    d.attr = clip(l["attr"] | "", 32);
    d.fmt = clip(l["fmt"] | "", 64);
    d.icon = clip(l["icon"] | "", ICON_NAME_MAX);
    d.color = parse_hex_color(l["color"] | "", i == 0 ? 0xFFD200 : 0xFFFFFF);
    d.unit = l["unit"] | true;
  }
  return true;
}

std::string MatrixWeb::informers_json_() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  std::string out = "[";
  for (size_t i = 0; i < this->informers_.size(); i++) {
    if (i)
      out += ',';
    informer_json(out, this->informers_[i]);
  }
  out += ']';
  return out;
}

// ---------------------------------------------------------------------------
// Флеш
// ---------------------------------------------------------------------------
void MatrixWeb::load_informers_() {
  this->informer_pref_ = global_preferences->make_preference<InformerStore>(INFORMERS_HASH, true);
  auto st = std::make_unique<InformerStore>();
  if (!this->informer_pref_.load(st.get()))
    return;
  st->json[sizeof(st->json) - 1] = 0;
  JsonDocument doc = json::parse_json(std::string(st->json));
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    Informer inf;
    inf.id = o["id"] | "";
    if (inf.id.empty() || !parse_informer(o, inf))
      continue;
    const int n = atoi(inf.id.c_str() + (inf.id.rfind("inf_", 0) == 0 ? 4 : 0));
    this->informer_seq_ = std::max(this->informer_seq_, n + 1);
    this->informers_.push_back(inf);
    if (std::find(this->order_.begin(), this->order_.end(), inf.id) == this->order_.end())
      this->order_.push_back(inf.id);
  }
  for (const auto &inf : this->informers_)
    this->watch_informer_(inf);
  ESP_LOGD(TAG, "Loaded %u informer(s)", (unsigned) this->informers_.size());
}

void MatrixWeb::save_informers_() {
  const std::string json = this->informers_json_();
  if (json.size() >= sizeof(InformerStore::json)) {
    ESP_LOGE(TAG, "Informers do not fit into flash slot (%u bytes)", (unsigned) json.size());
    return;
  }
  auto st = std::make_unique<InformerStore>();
  memset(st.get(), 0, sizeof(InformerStore));
  memcpy(st->json, json.data(), json.size());
  this->informer_pref_.save(st.get());
  global_preferences->sync();
}

// ---------------------------------------------------------------------------
// Зміни (основний цикл)
// ---------------------------------------------------------------------------
Informer *MatrixWeb::find_informer_(const std::string &id) {
  for (auto &inf : this->informers_)
    if (inf.id == id)
      return &inf;
  return nullptr;
}

void MatrixWeb::watch_informer_(const Informer &inf) {
  for (const auto &l : inf.line) {
    if (l.entity.empty())
      continue;
    this->watch_ha(l.entity, l.attr, nullptr);
    if (l.attr.empty())  // одиниця є лише в стану сенсора, не в атрибутах
      this->watch_ha(l.entity, "unit_of_measurement", nullptr);
  }
}

// Текст рядків для попереднього перегляду у веб-панелі (читається з потоку httpd під lock_)
void MatrixWeb::update_informer_previews_() {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  for (auto &inf : this->informers_)
    for (int i = 0; i < 2; i++)
      inf.preview[i] = this->informer_text(inf, i);
}

void MatrixWeb::upsert_informer_(Informer inf) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  if (auto *old = this->find_informer_(inf.id)) {
    *old = inf;
  } else {
    if ((int) this->informers_.size() >= INFORMER_MAX) {
      ESP_LOGW(TAG, "Too many informers");
      return;
    }
    this->informers_.push_back(inf);
    if (std::find(this->order_.begin(), this->order_.end(), inf.id) == this->order_.end()) {
      this->order_.push_back(inf.id);
      this->save_order_();
    }
  }
  this->watch_informer_(inf);
  this->save_informers_();
  ESP_LOGI(TAG, "Informer '%s' saved", inf.id.c_str());
}

void MatrixWeb::remove_informer_(const std::string &id) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const size_t before = this->informers_.size();
  this->informers_.erase(std::remove_if(this->informers_.begin(), this->informers_.end(),
                                        [&](const Informer &i) { return i.id == id; }),
                         this->informers_.end());
  if (this->informers_.size() == before)
    return;
  this->save_informers_();
  this->remove_pushed_(id);  // прибирає з черги (і перемикає, якщо показувався)
  ESP_LOGI(TAG, "Informer '%s' removed", id.c_str());
}

// ---------------------------------------------------------------------------
// Текст рядка: {v} — значення як є, {v0} {v1} {v2} — число з 0/1/2 знаками
// ---------------------------------------------------------------------------
std::string MatrixWeb::informer_text(const Informer &inf, int line) const {
  const auto &l = inf.line[line];
  std::string fmt = l.fmt;
  if (fmt.empty())
    fmt = l.entity.empty() ? "" : "{v}";
  std::string raw = "—";
  bool have = false;
  if (!l.entity.empty()) {
    if (const std::string *v = this->ha_value(l.entity, l.attr)) {
      if (!v->empty() && *v != "unavailable" && *v != "unknown") {
        raw = *v;
        have = true;
      }
    }
  }
  const auto num = have ? parse_number<float>(raw) : optional<float>{};
  // Одиниця виміру: {u} — явно; інакше (галочка unit) — одразу після кожного значення
  std::string unit;
  if (have && l.attr.empty())
    if (const std::string *u = this->ha_value(l.entity, "unit_of_measurement"))
      unit = *u;
  const bool auto_unit = l.unit && fmt.find("{u}") == std::string::npos;
  std::string out;
  out.reserve(fmt.size() + 16);
  for (size_t i = 0; i < fmt.size();) {
    if (fmt[i] == '{') {
      const size_t end = fmt.find('}', i);
      if (end != std::string::npos) {
        const std::string key = fmt.substr(i + 1, end - i - 1);
        if (key == "v") {
          out += raw;
          if (auto_unit)
            out += unit;
          i = end + 1;
          continue;
        }
        if (key == "u") {
          out += unit;
          i = end + 1;
          continue;
        }
        if (key.size() == 2 && key[0] == 'v' && key[1] >= '0' && key[1] <= '2') {
          if (num.has_value()) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%.*f", key[1] - '0', *num);
            out += buf;
          } else {
            out += raw;
          }
          if (auto_unit)
            out += unit;
          i = end + 1;
          continue;
        }
      }
    }
    out += fmt[i++];
  }
  return out;  // {time} {date} … підставляє lambda дисплея (matrix_gfx::expand)
}

}  // namespace esphome::matrix_web
