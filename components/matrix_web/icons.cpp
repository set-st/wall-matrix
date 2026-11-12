// Бібліотека іконок: формат, збереження у флеші (NVS через preferences), міграція.
#include "matrix_web.h"

#include <algorithm>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.icons";

// Індекс бібліотеки у флеші: імена та розміри блобів
struct IconIndex {
  uint8_t count;
  uint8_t reserved[3];
  char names[ICON_MAX_COUNT][ICON_NAME_MAX + 1];
  uint32_t sizes[ICON_MAX_COUNT];
};
static constexpr uint32_t ICON_INDEX_HASH = 0x57A11C0Cu;

// Стара єдина іконка (до бібліотеки) — переносимо під іменем "text"
struct LegacyIconStore {
  uint8_t frames;
  uint8_t reserved[3];
  uint16_t delay_ms[16];
  uint8_t data[16 * 16 * 16 * 3];
};
static constexpr uint32_t LEGACY_ICON_HASH = 0x57A11C0Bu;

static uint32_t icon_hash(const std::string &name) { return fnv1_hash("mwicon:" + name); }

// ---------------------------------------------------------------------------
// Icon
// ---------------------------------------------------------------------------
const uint8_t *Icon::frame_at(uint32_t now_ms) const {
  const int n = this->frames();
  if (n == 0)
    return nullptr;
  if (n == 1)
    return this->rgb.data();
  uint32_t total = 0;
  for (auto d : this->delays)
    total += std::max<uint32_t>(d, 20);
  uint32_t t = now_ms % total;
  for (int i = 0; i < n; i++) {
    const uint32_t d = std::max<uint32_t>(this->delays[i], 20);
    if (t < d)
      return this->rgb.data() + i * this->frame_bytes();
    t -= d;
  }
  return this->rgb.data();
}

std::string Icon::encode() const {
  std::string out;
  out.reserve(ICON_HEADER + this->frames() * (2 + this->frame_bytes()));
  out += 'W';
  out += 'M';
  out += (char) this->frames();
  out += (char) this->w;
  out += (char) this->h;
  out += (char) 0;
  for (int i = 0; i < this->frames(); i++) {
    out += (char) (this->delays[i] & 0xFF);
    out += (char) (this->delays[i] >> 8);
    out.append(reinterpret_cast<const char *>(this->rgb.data() + i * this->frame_bytes()), this->frame_bytes());
  }
  return out;
}

bool Icon::decode(const uint8_t *d, size_t len, Icon &out, std::string &err) {
  if (len < ICON_HEADER || d[0] != 'W' || d[1] != 'M') {
    err = "expected 'WM' header";
    return false;
  }
  const int frames = d[2], w = d[3], h = d[4];
  if (frames < 1 || frames > ICON_MAX_FRAMES) {
    err = "frames must be 1..16";
    return false;
  }
  if (!((w == 8 && h == 8) || (w == 16 && h == 16))) {
    err = "size must be 8x8 or 16x16";
    return false;
  }
  const size_t fb = (size_t) w * h * 3;
  if (len != ICON_HEADER + frames * (2 + fb)) {
    err = "length does not match header";
    return false;
  }
  out.w = w;
  out.h = h;
  out.delays.resize(frames);
  out.rgb.resize(frames * fb);
  size_t p = ICON_HEADER;
  for (int i = 0; i < frames; i++) {
    out.delays[i] = d[p] | (d[p + 1] << 8);
    memcpy(out.rgb.data() + i * fb, d + p + 2, fb);
    p += 2 + fb;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Бібліотека
// ---------------------------------------------------------------------------
const Icon *MatrixWeb::icon(const std::string &name) const {
  if (name.empty())
    return nullptr;
  for (const auto &ic : this->icons_)
    if (ic->name == name)
      return ic.get();
  return nullptr;
}

size_t MatrixWeb::icons_bytes_() const {
  size_t total = 0;
  for (const auto &ic : this->icons_)
    total += ICON_HEADER + ic->frames() * (2 + ic->frame_bytes());
  return total;
}

void MatrixWeb::load_icons_() {
  this->icon_index_pref_ = global_preferences->make_preference<IconIndex>(ICON_INDEX_HASH, true);
  auto index = std::make_unique<IconIndex>();
  if (this->icon_index_pref_.load(index.get()) && index->count <= ICON_MAX_COUNT) {
    for (int i = 0; i < index->count; i++) {
      index->names[i][ICON_NAME_MAX] = 0;
      const std::string name = index->names[i];
      const uint32_t size = index->sizes[i];
      if (name.empty() || size < ICON_HEADER || size > ICON_MAX_BYTES)
        continue;
      std::vector<uint8_t> buf(size);
      auto pref = global_preferences->make_preference(size, icon_hash(name), true);
      std::string err;
      auto ic = std::make_shared<Icon>();
      if (pref.load(buf.data(), size) && Icon::decode(buf.data(), size, *ic, err)) {
        ic->name = name;
        this->icons_.push_back(ic);
      } else {
        ESP_LOGW(TAG, "Icon '%s' could not be loaded", name.c_str());
      }
    }
  }

  // Міграція старої єдиної іконки
  if (this->icon("text") == nullptr) {
    auto legacy = std::make_unique<LegacyIconStore>();
    auto pref = global_preferences->make_preference<LegacyIconStore>(LEGACY_ICON_HASH, true);
    if (pref.load(legacy.get()) && legacy->frames > 0 && legacy->frames <= 16) {
      auto ic = std::make_shared<Icon>();
      ic->name = "text";
      ic->w = ic->h = 16;
      ic->delays.assign(legacy->delay_ms, legacy->delay_ms + legacy->frames);
      ic->rgb.assign(legacy->data, legacy->data + legacy->frames * 16 * 16 * 3);
      this->put_icon_(ic);
      legacy->frames = 0;
      pref.save(legacy.get());
      ESP_LOGI(TAG, "Migrated legacy icon as 'text'");
    }
  }
  ESP_LOGD(TAG, "Loaded %u icon(s), %u bytes", (unsigned) this->icons_.size(), (unsigned) this->icons_bytes_());
}

void MatrixWeb::save_icon_index_() {
  auto index = std::make_unique<IconIndex>();
  memset(index.get(), 0, sizeof(IconIndex));
  index->count = std::min<size_t>(this->icons_.size(), ICON_MAX_COUNT);
  for (int i = 0; i < index->count; i++) {
    strncpy(index->names[i], this->icons_[i]->name.c_str(), ICON_NAME_MAX);
    const auto &ic = *this->icons_[i];
    index->sizes[i] = ICON_HEADER + ic.frames() * (2 + ic.frame_bytes());
  }
  this->icon_index_pref_.save(index.get());
}

void MatrixWeb::put_icon_(std::shared_ptr<Icon> ic) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  const std::string blob = ic->encode();
  auto pref = global_preferences->make_preference(blob.size(), icon_hash(ic->name), true);
  pref.save(reinterpret_cast<const uint8_t *>(blob.data()), blob.size());
  bool replaced = false;
  for (auto &old : this->icons_)
    if (old->name == ic->name) {
      old = ic;
      replaced = true;
    }
  if (!replaced)
    this->icons_.push_back(ic);
  this->save_icon_index_();
  global_preferences->sync();
  ESP_LOGI(TAG, "Icon '%s' saved: %dx%d, %d frame(s)", ic->name.c_str(), ic->w, ic->h, ic->frames());
}

void MatrixWeb::remove_icon_(const std::string &name) {
  std::lock_guard<std::recursive_mutex> guard(this->lock_);
  auto it = std::remove_if(this->icons_.begin(), this->icons_.end(),
                           [&](const std::shared_ptr<Icon> &ic) { return ic->name == name; });
  if (it == this->icons_.end())
    return;
  this->icons_.erase(it, this->icons_.end());
  // Звільняємо місце у флеші: перезаписуємо блоб одним байтом
  uint8_t zero = 0;
  auto pref = global_preferences->make_preference(1, icon_hash(name), true);
  pref.save(&zero, 1);
  this->save_icon_index_();
  global_preferences->sync();
  ESP_LOGI(TAG, "Icon '%s' removed", name.c_str());
}

}  // namespace esphome::matrix_web
