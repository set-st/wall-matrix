#pragma once
// Графічні примітиви для матриці N×16 (ширина 16…64 px): великі цифри 5×14, годинник, дата,
// температура, сповіщення, два рядки тексту, іконка і тестовий екран.

#include "esphome.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace matrix_gfx {

using esphome::Color;
using esphome::ESPTime;
using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

// Цифри 5×7; біт 4 — лівий стовпець. Під час виводу кожен рядок подвоюється → 5×14.
static const uint8_t DIGITS_5X7[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

constexpr int DIGIT_W = 5;
constexpr int DIGIT_H = 14;

// Поточна ширина матриці; виставляє lambda дисплея на кожному кадрі (висота завжди 16).
inline int W = 32;

// Група з чотирьох великих цифр «ГГ:ХХ» має ширину 28 px; на ширших матрицях — по центру.
// Відносні позиції цифр: [0..4] [6..10] : [13..14] [17..21] [23..27]
constexpr int BIG_W = 28;
constexpr int POS[4] = {0, 6, 17, 23};
inline int big_x0() { return (W - BIG_W) / 2; }
inline bool big_fits() { return W >= 32; }

// "#RRGGBB" / "RRGGBB" → 0xRRGGBB; у разі помилки — fallback
inline uint32_t parse_hex(const std::string &s, uint32_t fallback) {
  const char *p = s.c_str();
  if (*p == '#') p++;
  if (strlen(p) != 6) return fallback;
  char *end = nullptr;
  const uint32_t v = strtoul(p, &end, 16);
  return (end && *end == '\0') ? v : fallback;
}

inline Color hex_color(const std::string &s, uint32_t fallback) { return Color(parse_hex(s, fallback)); }

// Гамма 2.8: світлодіоди лінійні, око — ні. Вбудовану гамму ESPHome для матриці
// вимкнено (gamma_correct: 1.0), бо вона застосовується ПІСЛЯ масштабування яскравості
// і з'їдає глибину кольору. Тож гамму робимо тут, а яскравість/обмеження струму —
// лінійно через корекцію стрічки.
inline uint8_t gamma8(uint8_t v) {
  static uint8_t table[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; i++) {
      const float g = powf(i / 255.0f, 2.8f) * 255.0f + 0.5f;
      table[i] = (i > 0 && g < 1.0f) ? 1 : (uint8_t) g;  // ненульове не гасимо повністю
    }
    ready = true;
  }
  return table[v];
}

// Колір для виводу: множник k (0..1) у «перцептивному» просторі + гамма
inline Color dim(Color c, float k) {
  if (k < 0.0f) k = 0.0f;
  if (k > 1.0f) k = 1.0f;
  return Color(gamma8((uint8_t) (c.r * k)), gamma8((uint8_t) (c.g * k)), gamma8((uint8_t) (c.b * k)));
}

// Приглушити вже підготовлений (лінійний) колір — без повторної гамми
inline Color fade(Color c, float f) {
  return Color((uint8_t) (c.r * f), (uint8_t) (c.g * f), (uint8_t) (c.b * f));
}

inline void big_digit(Display &it, int x, int y, int d, Color c) {
  if (d < 0 || d > 9) return;
  for (int r = 0; r < 7; r++)
    for (int col = 0; col < 5; col++)
      if (DIGITS_5X7[d][r] & (0x10 >> col))
        it.filled_rectangle(x + col, y + r * 2, 1, 2, c);
}

inline void two_pairs(Display &it, int a, int b, Color c) {
  const int o = big_x0();
  big_digit(it, o + POS[0], 0, a / 10, c);
  big_digit(it, o + POS[1], 0, a % 10, c);
  big_digit(it, o + POS[2], 0, b / 10, c);
  big_digit(it, o + POS[3], 0, b % 10, c);
}

inline void placeholder(Display &it, Color c) {
  const int o = big_x0();
  for (int x : POS) it.filled_rectangle(o + x, 6, DIGIT_W, 2, c);
}

// Вузькі матриці (16–24 px): текст високим шрифтом в один рядок, а якщо не влазить —
// дрібним у два рядки (a зверху, b знизу). Повертає true, якщо вийшло одним рядком.
inline bool narrow_pair(Display &it, BaseFont *tall, BaseFont *small, const char *a, const char *sep,
                        const char *b, Color c) {
  char one[16];
  snprintf(one, sizeof(one), "%s%s%s", a, sep, b);
  int x1, y1, tw, th;
  if (tall != nullptr) {
    it.get_text_bounds(0, 0, one, tall, TextAlign::TOP_LEFT, &x1, &y1, &tw, &th);
    if (tw <= W) {
      it.print((W - tw) / 2, 0, tall, c, TextAlign::TOP_LEFT, one);
      return true;
    }
  }
  if (small != nullptr) {
    it.get_text_bounds(0, 0, a, small, TextAlign::TOP_LEFT, &x1, &y1, &tw, &th);
    it.print((W - tw) / 2, 0, small, c, TextAlign::TOP_LEFT, a);
    it.get_text_bounds(0, 0, b, small, TextAlign::TOP_LEFT, &x1, &y1, &tw, &th);
    it.print((W - tw) / 2, 8, small, c, TextAlign::TOP_LEFT, b);
  }
  return false;
}

// Годинник ГГ:ХХ, двокрапка (блимає або постійна), знизу смужка секунд.
inline void draw_clock(Display &it, const ESPTime &t, Color c, bool h24 = true, bool blink = true,
                       bool seconds_bar = true, BaseFont *tall = nullptr, BaseFont *small = nullptr) {
  int hour = t.hour;
  if (!h24) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  bool bar_ok = true;
  if (!big_fits()) {
    char hh[4] = "--", mm[4] = "--";
    if (t.is_valid()) {
      snprintf(hh, sizeof(hh), "%02d", hour);
      snprintf(mm, sizeof(mm), "%02d", t.minute);
    }
    const bool colon = !blink || t.second % 2 == 0;
    bar_ok = narrow_pair(it, tall, small, hh, colon ? ":" : " ", mm, c);
  } else if (!t.is_valid()) {
    placeholder(it, c);
    return;
  } else {
    two_pairs(it, hour, t.minute, c);
    if (!blink || t.second % 2 == 0) {
      const int o = big_x0();
      it.filled_rectangle(o + 13, 3, 2, 2, c);
      it.filled_rectangle(o + 13, 9, 2, 2, c);
    }
  }
  if (seconds_bar && bar_ok && t.is_valid()) {
    it.horizontal_line(0, 15, W, fade(c, 0.08f));
    it.horizontal_line(0, 15, (t.second + 1) * W / 60, c);
  }
}

// Дата ДД.ММ, знизу індикатор дня тижня (Пн…Нд або Нд…Сб).
inline void draw_date(Display &it, const ESPTime &t, Color c, Color accent, bool monday_first = true,
                      BaseFont *tall = nullptr, BaseFont *small = nullptr) {
  if (!big_fits()) {
    char dd[4] = "--", mm[4] = "--";
    if (t.is_valid()) {
      snprintf(dd, sizeof(dd), "%02d", t.day_of_month);
      snprintf(mm, sizeof(mm), "%02d", t.month);
    }
    narrow_pair(it, tall, small, dd, ".", mm, c);
    return;
  }
  if (!t.is_valid()) {
    placeholder(it, c);
    return;
  }
  two_pairs(it, t.day_of_month, t.month, c);
  it.filled_rectangle(big_x0() + 13, 12, 2, 2, c);
  // Індикатор днів тижня: 7 відрізків по 3 px через 1 (27 px), по центру.
  // day_of_week: 1 = Нд … 7 = Сб
  const int today = monday_first ? (t.day_of_week + 5) % 7 : t.day_of_week - 1;
  const int wx = (W - 27) / 2;
  for (int i = 0; i < 7; i++)
    it.horizontal_line(wx + i * 4, 15, 3, i == today ? accent : fade(c, 0.08f));
}

// Температура великими цифрами, по центру області [x0, x0 + area_w).
inline void draw_temperature(Display &it, int x0, int area_w, float value, Color c) {
  if (std::isnan(value)) {
    it.filled_rectangle(x0 + (area_w - DIGIT_W) / 2, 7, DIGIT_W, 2, c);
    return;
  }
  int v = (int) lroundf(value);
  const bool neg = v < 0;
  if (neg) v = -v;
  if (v > 99) v = 99;
  const int n = v >= 10 ? 2 : 1;
  int w = n * DIGIT_W + (n - 1) + (neg ? 3 : 0);
  const bool deg = w + 3 <= area_w;  // знак градуса, якщо вміщається
  if (deg) w += 3;

  int x = x0 + (area_w - w) / 2;
  if (neg) {
    it.filled_rectangle(x, 7, 2, 2, c);
    x += 3;
  }
  if (n == 2) {
    big_digit(it, x, 1, v / 10, c);
    x += DIGIT_W + 1;
  }
  big_digit(it, x, 1, v % 10, c);
  x += DIGIT_W;
  if (deg) it.filled_rectangle(x + 1, 1, 2, 2, c);
}

// Сповіщення: кольорова рамка згори/знизу, текст по центру або рядком, що біжить.
// scroll_pos — поточна позиція, step — зсув за кадр у пікселях.
inline void draw_notification(Display &it, BaseFont *font, const std::string &text, Color frame,
                              Color fg, float &scroll_pos, float step) {
  it.horizontal_line(0, 0, W, frame);
  it.horizontal_line(0, 15, W, frame);
  int x1, y1, w, h;
  it.get_text_bounds(0, 0, text.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
  if (w <= W) {
    it.print(W / 2, 8, font, fg, TextAlign::CENTER, text.c_str());
    return;
  }
  if (scroll_pos > W)
    scroll_pos = W;
  it.print((int) scroll_pos, 8, font, fg, TextAlign::CENTER_LEFT, text.c_str());
  scroll_pos -= step;
  if (scroll_pos < -w) scroll_pos = W;
}

// Тестовий екран: кожна панель своїм кольором (веселка зліва направо, згори вниз),
// білий піксель у лівому верхньому куті кожної панелі, біла діагональ через увесь екран.
inline void draw_test(Display &it, float k) {
  static const Color TILE[8] = {
      Color(255, 0, 0),   Color(255, 120, 0), Color(255, 255, 0), Color(0, 255, 0),
      Color(0, 255, 255), Color(0, 0, 255),   Color(160, 0, 255), Color(255, 0, 160),
  };
  const int cols = W / 8;
  for (int i = 0; i < cols * 2; i++) {
    const int tx = (i % cols) * 8, ty = (i / cols) * 8;
    it.filled_rectangle(tx, ty, 8, 8, dim(TILE[i % 8], k * 0.3f));
    it.draw_pixel_at(tx, ty, dim(Color(255, 255, 255), k));
  }
  it.line(0, 0, W - 1, 15, dim(Color(255, 255, 255), k));
}

// ---------------------------------------------------------------------------
// Текстовий застосунок: два рядки дрібним шрифтом + іконка
// ---------------------------------------------------------------------------

// Підстановки в рядках: {time} {date} {day} {temp}
inline std::string expand(const std::string &src, const ESPTime &t, float temp) {
  static const char *const DAYS[7] = {"Нд", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
  std::string out;
  out.reserve(src.size() + 8);
  for (size_t i = 0; i < src.size();) {
    if (src[i] == '{') {
      const size_t end = src.find('}', i);
      if (end != std::string::npos) {
        const std::string key = src.substr(i + 1, end - i - 1);
        char buf[16] = {0};
        bool ok = true;
        if (key == "time")
          t.is_valid() ? snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute) : snprintf(buf, sizeof(buf), "--:--");
        else if (key == "date")
          t.is_valid() ? snprintf(buf, sizeof(buf), "%02d.%02d", t.day_of_month, t.month) : snprintf(buf, sizeof(buf), "--.--");
        else if (key == "day")
          snprintf(buf, sizeof(buf), "%s", t.is_valid() ? DAYS[(t.day_of_week + 6) % 7] : "--");
        else if (key == "temp")
          std::isnan(temp) ? snprintf(buf, sizeof(buf), "--°") : snprintf(buf, sizeof(buf), "%d°", (int) lroundf(temp));
        else
          ok = false;
        if (ok) {
          out += buf;
          i = end + 1;
          continue;
        }
      }
    }
    out += src[i++];
  }
  return out;
}

// Великі літери для UTF-8: латиниця, кирилиця (рос./укр., зокрема ґ є і ї ё).
inline std::string to_upper(const std::string &src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); i++) {
    const uint8_t c = src[i];
    if (c < 0x80) {
      out += (c >= 'a' && c <= 'z') ? (char) (c - 32) : (char) c;
      continue;
    }
    if ((c == 0xD0 || c == 0xD1 || c == 0xD2) && i + 1 < src.size()) {
      uint32_t cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F);
      if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;        // а-я → А-Я
      else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;   // ѐ-џ (ё є і ї …) → Ѐ-Џ
      else if (cp == 0x491) cp = 0x490;                  // ґ → Ґ
      out += (char) (0xC0 | (cp >> 6));
      out += (char) (0x80 | (cp & 0x3F));
      i++;
      continue;
    }
    out += (char) c;
  }
  return out;
}

// Пауза перед прокручуванням і зазор між повтореннями — як в AWTRIX
static constexpr float SCROLL_PAUSE_PX = 20.0f;  // ≈ 1 с при швидкості 20 px/s
static constexpr int SCROLL_GAP = 8;

// Рядок у прямокутнику [x0, x0+w) × [y, y+8): якщо влазить — по центру, інакше
// після паузи біжить по колу (видно одразу, без «в'їзду» з краю).
// pos > 0 — ще триває пауза; pos ≤ 0 — зсув тексту.
inline void draw_line(Display &it, BaseFont *font, const std::string &text, int x0, int y, int w, Color c,
                      float &pos, float step, int h = 8) {
  if (text.empty())
    return;
  int x1, y1, tw, th;
  it.get_text_bounds(0, 0, text.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &tw, &th);
  it.start_clipping(x0, y, x0 + w, y + h);
  if (tw <= w) {
    it.print(x0 + (w - tw) / 2, y, font, c, TextAlign::TOP_LEFT, text.c_str());
  } else {
    const int period = tw + SCROLL_GAP;
    const int off = pos > 0 ? 0 : (int) pos;
    it.print(x0 + off, y, font, c, TextAlign::TOP_LEFT, text.c_str());
    if (off + period < w)
      it.print(x0 + off + period, y, font, c, TextAlign::TOP_LEFT, text.c_str());
    pos -= step;
    if (pos <= -period)
      pos = SCROLL_PAUSE_PX;  // повний оберт — знову пауза на початку
  }
  it.end_clipping();
}

// RGB-картинка (w×h, по 3 байти на піксель); чорні пікселі — прозорі.
inline void draw_rgb(Display &it, int x, int y, int w, int h, const uint8_t *rgb, float k) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      const uint8_t *p = rgb + (yy * w + xx) * 3;
      if (p[0] | p[1] | p[2])
        it.draw_pixel_at(x + xx, y + yy, dim(Color(p[0], p[1], p[2]), k));
    }
}

// Інформер. Два рядки дрібним шрифтом або один — високим (на всю висоту).
// Іконка рядка 1: 16×16 займає обидва рядки (текст праворуч), 8×8 — лише свій рядок;
// іконка рядка 2 — лише 8×8. В режимі одного рядка — іконка рядка 1 по центру зліва.
inline void draw_informer(Display &it, BaseFont *small, BaseFont *tall, bool single, const std::string &t1,
                          const std::string &t2, Color c1, Color c2, const uint8_t *ic1, int ic1w, int ic1h,
                          const uint8_t *ic2, int ic2w, int ic2h, float step, float &p0, float &p1) {
  if (single) {
    int x0 = 0;
    if (ic1 != nullptr) {
      draw_rgb(it, 0, (16 - ic1h) / 2, ic1w, ic1h, ic1, 1.0f);
      x0 = ic1w + 1;
    }
    draw_line(it, tall, t1, x0, 0, W - x0, c1, p0, step, 16);
    return;
  }
  if (ic1 != nullptr && ic1h >= 16) {
    draw_rgb(it, 0, 0, ic1w, ic1h, ic1, 1.0f);
    draw_line(it, small, t1, ic1w + 1, 0, W - 1 - ic1w, c1, p0, step);
    draw_line(it, small, t2, ic1w + 1, 8, W - 1 - ic1w, c2, p1, step);
    return;
  }
  int x = 0;
  if (ic1 != nullptr) {
    draw_rgb(it, 0, 0, ic1w, ic1h, ic1, 1.0f);
    x = ic1w + 1;
  }
  draw_line(it, small, t1, x, 0, W - x, c1, p0, step);
  x = 0;
  if (ic2 != nullptr && ic2h <= 8) {
    draw_rgb(it, 0, 8, ic2w, ic2h, ic2, 1.0f);
    x = ic2w + 1;
  }
  draw_line(it, small, t2, x, 8, W - x, c2, p1, step);
}

// Застосунок / сповіщення з іконкою: іконка зліва (по центру по вертикалі), праворуч текст.
// Текст: один рядок — по центру по вертикалі; "\n" — два рядки. progress ≥ 0 — смужка внизу.
inline void draw_app(Display &it, BaseFont *font, const uint8_t *icon, int iw, int ih, const std::string &text,
                     Color c, int progress, Color pc, float step, float &p0, float &p1) {
  int x0 = 0;
  if (icon != nullptr) {
    draw_rgb(it, 0, (16 - ih) / 2, iw, ih, icon, 1.0f);
    x0 = iw + 1;
  }
  const int w = W - x0;
  const size_t nl = text.find('\n');
  if (nl == std::string::npos) {
    draw_line(it, font, text, x0, progress >= 0 ? 2 : 4, w, c, p0, step);
  } else {
    draw_line(it, font, text.substr(0, nl), x0, 0, w, c, p0, step);
    draw_line(it, font, text.substr(nl + 1), x0, 8, w, c, p1, step);
  }
  if (progress >= 0) {
    it.horizontal_line(x0, 15, w, fade(pc, 0.12f));
    it.horizontal_line(x0, 15, (w * std::min(progress, 100) + 50) / 100, pc);
  }
}

}  // namespace matrix_gfx
