# Wall Matrix — пам'ять проєкту

AWTRIX-подібна настінна панель на ESPHome. Загальний опис, схема й API — у README.md.
Мова проєкту (інтерфейс, документація, коментарі) — **українська**.

## Залізо (перевірено)
- Плата **YD-ESP32-23 2022-v1.3**, ESP32-S3 **N16R8** (16 МБ флеш, 8 МБ октальна PSRAM).
- Матриця 32×16: 8 панелей WS2812B 8×8, сітка 4×2, одна лінія даних.
- **Матриця підключена після вбудованого WS2812 (GPIO48)**: його DOUT → 330 Ом → DIN панелі 0.
  Вбудований LED живиться від **4.73 В** (заміряно), тому сам піднімає рівень сигналу —
  перетворювач рівнів не потрібен. Стрічка `led_strip` = 513 пікселів, ділиться `partition`:
  `status_led` — піксель 0 (червоний = немає Wi-Fi, через wifi on_connect/on_disconnect),
  `matrix_light` — 1..512, на нього малює display (`pixel_mapper` без зсуву).
- Порядок панелей і `panel_serpentine` ще **не перевірені на реальній матриці** (панелі не
  припаяні на момент запису) — перевіряти тестовим екраном. Програмно карту перевірено через
  /api/live: діагональ і кутові точки панелей на місці.

## Мережа та прошивка
- Пристрій: `wall-matrix`, IP **192.168.1.192**. Звичайна прошивка — по Wi-Fi:
  `esphome run wall-matrix.yaml --device 192.168.1.192 --no-logs`
- Логи мережею: `esphome logs wall-matrix.yaml --device 192.168.1.192`.
  Логер іде в USB_SERIAL_JTAG (роз'єм «USB»), а не в COM (CH343) — по COM видно лише завантажувач.
- Хост — **WSL2**: USB у WSL не прокинуто (`usbipd bind` потребує адміна). Прошивка дротом —
  через Windows Python: `py -m esptool --chip esp32s3 --port COM3 --baud 460800 write_flash 0x0 firmware.factory.bin`.
  esptool стоїть як **4.8.1 з `--no-deps`** (+ pyserial, intelhex): esptool 5 тягне PyYAML,
  який не збирається під Windows Python 3.15.
- Браузер: підключено Claude in Chrome (Windows). Користувач сам авторизований у панелі —
  пароль не вводити. Клікати за `ref` (find), а не координатами: сторінка зсувається після
  перемикання вкладок (так якось випадково змінилася яскравість).
- Оригінальний AWTRIX NG користувача: http://192.168.2.101/ — звідти взято візуальний стиль
  (палітра в :root у web/index.html). У його налаштуваннях нічого не змінювати.
- Репозиторій: https://github.com/set-st/wall-matrix — **пушить користувач сам**.

## Веб-панель
- **Власна** (штатний `web_server` ESPHome видалено, бо займає «/»): компонент
  `components/matrix_web` + сторінка `web/index.html` (gzip, вшивається під час збирання).
  Digest-auth, логін/пароль — `web_*` у secrets.yaml.
- Сутності для API перелічуються за YAML id у блоці `matrix_web:` у packages/admin.yaml —
  нове налаштування додавати туди **і** в `PAGES` в index.html.
- Перевірка картинки без матриці: завантажити /api/live і відмалювати (так знайшовся
  «залиплий» піксель і зламана глибина кольору).
- Іконка: 16×16, до 16 кадрів, зберігається в NVS через preferences (`IconStore` ~12 КБ),
  кадр для екрана — `id(web_panel)->icon_frame(millis())`.

## Підводні камені ESPHome (2026.10)
- У ESP32-S3 лише **192 символи RMT** на всі TX-канали. Дві стрічки типово беруть по 192 →
  друга падає з `marked FAILED`. Зараз один канал з `rmt_symbols: 192`.
- У `api: actions` ім'я змінної `text` зарезервоване — використовується `message`.
- Підстановки `${...}` усередині flow-списку `[a, b]` ламають YAML — писати блочним списком.
- `rgb_order` застарів → `channel_colors`; `image:` — з `platform: file`.
- Час: SNTP + homeassistant, обидва пишуть у спільний системний годинник; екран читає `id(ha_time)`.
  Часовий пояс `Europe/Kyiv` (з Windows «FLE Standard Time»).
- `addressable_light` display пише буфер W×H в індекси 0..W*H-1 світла і **відкидає**
  індекси pixel_mapper ≥ W*H — зсувати матрицю в стрічці через mapper не можна, лише partition.
- Веб-сервер IDF: обробники запитів працюють у задачі httpd — зміни сутностей робити через
  `defer()`; POST з не-form Content-Type приходить у handleBody (raw body).
- **Корекція ESPHome масштабує яскравість ДО гамми** (`gamma_table[scale(v)]`): з
  color_correct 40% і яскравістю 50% на канал лишалося 2–3 рівні. Тому для матриці
  `gamma_correct: 1.0`, гамму 2.8 робить `matrix_gfx::dim()`, а яскравість × `max_power`
  виставляється лінійно через `AddressableLight::set_correction()` у lambda (без спалаху,
  на відміну від light.turn_on). Уже готові (лінійні) кольори приглушувати `fade()`, не `dim()`.
- Імена сутностей — англійською навмисно: від них залежать ID у HA і ключі збереження у флеші.
- Шрифти: Press Start 2P (сповіщення) і Tiny5 size 8 (два рядки, піксель-у-піксель, є Ґ Є І Ї).

## Структура
- `wall-matrix.yaml` — substitutions + пакети.
- `packages/core.yaml` — плата, Wi-Fi, OTA, API (`notify`/`dismiss`/`set_text`).
- `packages/display.yaml` — стрічка, шрифти, іконки MDI, display, гортання 4 екранів.
- `packages/sensors.yaml` — час, погода з HA (`weather_entity`), фоторезистор GPIO4.
- `packages/admin.yaml` — усі налаштування (template-сутності), скрипти `notify_show` / `next_screen`, блок `matrix_web`.
- `components/matrix_web/` — власний HTTP-обробник поверх web_server_base (C++ + codegen).
- `web/index.html` — веб-панель у стилі AWTRIX NG, vanilla JS.
- `includes/matrix_gfx.h` — цифри 5×14, годинник, дата, температура, текст, іконка, гамма, тест.

## Користувач
Пише російською (відповідати російською), а проєкт веде українською. Любить робити по кроках
і перевіряти на залізі. Зовнішній вигляд важливий: веб-панель має бути «як в AWTRIX».
