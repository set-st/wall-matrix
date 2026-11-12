#pragma once
// Графічні примітиви для матриці 32×16: великі цифри 5×14, годинник, дата, температура,
// сповіщення, два рядки тексту, іконка і тестовий екран.

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

// Позиції чотирьох великих цифр: [2..6] [8..12] : [15..16] [19..23] [25..29]
constexpr int POS[4] = {2, 8, 19, 25};

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
  big_digit(it, POS[0], 0, a / 10, c);
  big_digit(it, POS[1], 0, a % 10, c);
  big_digit(it, POS[2], 0, b / 10, c);
  big_digit(it, POS[3], 0, b % 10, c);
}

inline void placeholder(Display &it, Color c) {
  for (int x : POS) it.filled_rectangle(x, 6, DIGIT_W, 2, c);
}

// Годинник ГГ:ХХ, двокрапка (блимає або постійна), знизу смужка секунд.
inline void draw_clock(Display &it, const ESPTime &t, Color c, bool h24 = true, bool blink = true,
                       bool seconds_bar = true) {
  if (!t.is_valid()) {
    placeholder(it, c);
    return;
  }
  int hour = t.hour;
  if (!h24) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  two_pairs(it, hour, t.minute, c);
  if (!blink || t.second % 2 == 0) {
    it.filled_rectangle(15, 3, 2, 2, c);
    it.filled_rectangle(15, 9, 2, 2, c);
  }
  if (seconds_bar) {
    it.horizontal_line(0, 15, 32, fade(c, 0.08f));
    it.horizontal_line(0, 15, (t.second + 1) * 32 / 60, c);
  }
}

// Дата ДД.ММ, знизу індикатор дня тижня (Пн…Нд або Нд…Сб).
inline void draw_date(Display &it, const ESPTime &t, Color c, Color accent, bool monday_first = true) {
  if (!t.is_valid()) {
    placeholder(it, c);
    return;
  }
  two_pairs(it, t.day_of_month, t.month, c);
  it.filled_rectangle(15, 12, 2, 2, c);
  // day_of_week: 1 = Нд … 7 = Сб
  const int today = monday_first ? (t.day_of_week + 5) % 7 : t.day_of_week - 1;
  for (int i = 0; i < 7; i++)
    it.horizontal_line(2 + i * 4, 15, 3, i == today ? accent : fade(c, 0.08f));
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
  it.horizontal_line(0, 0, 32, frame);
  it.horizontal_line(0, 15, 32, frame);
  int x1, y1, w, h;
  it.get_text_bounds(0, 0, text.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
  if (w <= 32) {
    it.print(16, 8, font, fg, TextAlign::CENTER, text.c_str());
    return;
  }
  it.print((int) scroll_pos, 8, font, fg, TextAlign::CENTER_LEFT, text.c_str());
  scroll_pos -= step;
  if (scroll_pos < -w) scroll_pos = 32;
}

// Тестовий екран: кожна панель своїм кольором (веселка зліва направо, згори вниз),
// білий піксель у лівому верхньому куті кожної панелі, біла діагональ через увесь екран.
inline void draw_test(Display &it, float k) {
  static const Color TILE[8] = {
      Color(255, 0, 0),   Color(255, 120, 0), Color(255, 255, 0), Color(0, 255, 0),
      Color(0, 255, 255), Color(0, 0, 255),   Color(160, 0, 255), Color(255, 0, 160),
  };
  for (int i = 0; i < 8; i++) {
    const int tx = (i % 4) * 8, ty = (i / 4) * 8;
    it.filled_rectangle(tx, ty, 8, 8, dim(TILE[i], k * 0.3f));
    it.draw_pixel_at(tx, ty, dim(Color(255, 255, 255), k));
  }
  it.line(0, 0, 31, 15, dim(Color(255, 255, 255), k));
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

// Рядок у прямокутнику [x0, x0+w) × [y, y+8): якщо влазить — по центру, інакше біжить.
inline void draw_line(Display &it, BaseFont *font, const std::string &text, int x0, int y, int w, Color c,
                      float &pos, float step) {
  if (text.empty())
    return;
  int x1, y1, tw, th;
  it.get_text_bounds(0, 0, text.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &tw, &th);
  it.start_clipping(x0, y, x0 + w, y + 8);
  if (tw <= w) {
    it.print(x0 + (w - tw) / 2, y, font, c, TextAlign::TOP_LEFT, text.c_str());
    pos = 0;
  } else {
    it.print(x0 + (int) pos, y, font, c, TextAlign::TOP_LEFT, text.c_str());
    pos -= step;
    if (pos < -tw) pos = w;
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

}  // namespace matrix_gfx
