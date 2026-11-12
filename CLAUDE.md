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
- Браузер: підключено Claude in Chrome (Windows). Паролів і токенів не вводити (токен HA для
  пошуку сутностей вводить користувач). Клікати за `ref` (find), а не координатами: сторінка зсувається після
  перемикання вкладок (так якось випадково змінилася яскравість).
- Оригінальний AWTRIX NG користувача: http://192.168.2.101/ — звідти взято візуальний стиль
  (палітра в :root у web/index.html). У його налаштуваннях нічого не змінювати.
- Репозиторій: https://github.com/set-st/wall-matrix — **пушить користувач сам**.

## Веб-панель
- **Власна** (штатний `web_server` ESPHome видалено, бо займає «/»): компонент
  `components/matrix_web` + сторінка `web/index.html` (gzip, вшивається під час збирання).
  **Без авторизації** — рішення користувача (локальна мережа). Компонент її підтримує:
  `username`/`password` у блоці `matrix_web:` вмикають digest для всіх маршрутів.
- Сутності для API перелічуються за YAML id у блоці `matrix_web:` у packages/admin.yaml —
  нове налаштування додавати туди **і** в `PAGES` в index.html.
- Перевірка картинки без матриці: завантажити /api/live і відмалювати (так знайшовся
  «залиплий» піксель і зламана глибина кольору).
- **Рушій застосунків** у компоненті (apps.cpp): черга `order_` (вбудовані Time/Date/Weather/Text +
  pushed з API), ротація в `loop()`, черга сповіщень. Display lambda лише питає
  `web->current_notification()` / `web->current_app()`; YAML-скрипти викликають `next_app()`,
  `switch_to("Text")`, `notify()`, `dismiss()`. Глобальних screen_idx/notify_* більше немає.
- **API у стилі AWTRIX NG** (api_v1.cpp), вибір користувача. Маршрути з'ясовано з UI його NG
  (http://192.168.2.101, лише GET-запити): apps, apps/order, apps/active, apps/next|previous,
  notifications (+DELETE active|name), display (GET/PATCH), display/screen. Створення pushed
  (`POST /api/v1/apps/<name>`) — наше розширення: у NG UI маршрут не видно. /api/v1/system не робити.
- ESPHome httpd реєструє лише GET/POST/OPTIONS — PUT/PATCH/DELETE реєструємо самі
  (`register_raw_methods_`, обгортка `RawRequest` для захищеного конструктора, digest-перевірка, лише якщо задано username).
- **Сутності HA під час роботи** (ha.cpp): `watch_ha(entity, attribute, cb)` — як у ~/panel
  (panel_cards.h `subscribe_key`): підписка один раз на пару, повторний виклик замінює обробник
  і віддає останнє значення; якщо HA вже підключений — шле йому `SubscribeHomeAssistantStateResponse`.
  Погода: text `weather_entity` (on_value → watch_ha) → template `weather_temp` / `weather_state`.
  Нові вбудовані застосунки з даними HA робити так само (text-сутність + watch_ha).
- Пошук сутностей у веб-панелі: браузер → WebSocket HA (`get_states`) з токеном у localStorage
  (`wall-matrix-ha`), на панель токен не йде. Поле `t: "entity"` у PAGES, `domains` — фільтр.
- **Інформери** (informers.cpp): застосунки користувача, `AppKind::INFORMER`, id `inf_N`, у черзі
  origin "informer". JSON у NVS (`InformerStore` 4 КБ, до 12 шт). Значення сутностей — кеш
  `ha_value()` (підписка `watch_ha(e, attr, nullptr)`: без обробника НЕ замінює чужий, напр. погоди).
  Малювання — `matrix_gfx::draw_informer`; високий шрифт `f_tall` = Handjet 14 (16/18 мають дефекти).
  POST /informers з тілом без `lines` → шаблон («Текст»).
  Одиниці: для рядка без атрибута ще підписка на `unit_of_measurement`; `unit` (типово true) —
  одиниця після кожного {v*}, якщо в шаблоні немає `{u}`. `preview[]` оновлюється в loop() раз на 1 с
  (під lock_), віддається в GET /informers; веб-панель оновлює лише «Зараз», не перебудовуючи картки.
- **Wi-Fi у веб-панелі** (`/api/v1/wifi`): стан, мережі (`request_wifi_scan_results` + `ScanResultsLock`,
  у codegen), збереження — `save_wifi_sta` через 500 мс. Сканування НЕ запускати при підключенні
  (`start_scanning` ламає стан STA). У режимі точки доступу `captive_portal` лишається заради DNS,
  але наш `canHandle` бере будь-який GET → телефон відкриває нашу панель. Перевірено лише в звичайному
  режимі; режим AP на залізі не тестувався. Пароль AP — `12345678` (рішення користувача).
- **Бібліотека іконок** (icons.cpp): до 32 шт, 256 КБ, NVS — блоб на іконку (`fnv1("mwicon:"+name)`)
  + індекс; видалення перезаписує блоб 1 байтом. Формат 'W''M' N W H 0 + кадри.
  Стару єдину іконку переносить як `text` (міграція).

## Підводні камені ESPHome (2026.10)
- **Сокети!** Власний компонент замість `web_server` мусить резервувати їх сам
  (`socket.consume_sockets(7, "matrix_web")` у `CONFIG_SCHEMA`). Без цього було
  `CONFIG_LWIP_MAX_SOCKETS=12`, і відкрита веб-панель (keep-alive, до 6 з'єднань на вкладку)
  забирала всі: старі з'єднання жили, нові (API HA, OTA, HTTP) отримували відмову, ping працював —
  «напівзависання» без перезавантаження. Тепер 19 сокетів + веб-панель робить ≤ 2 запити одночасно
  (`netRun` у index.html) — нові запити до панелі робити лише через `api()`/`post()`/`netRun`.
  Якщо знову «нові з'єднання відмовляють» — перевірити це першим; плату скинути через RTS на COM3.
- `ImageDecoder` (WebCodecs) є лише на HTTPS/localhost — на `http://<IP>` його немає. GIF
  розбирає власний декодер у index.html (`decodeGif`, LZW, disposal, прозорість).
- Імпорт іконок: LaMetric `https://developer.lametric.com/content/apps/icon_thumbs/<id>`
  (PNG/GIF 8×8, CORS *), пошук — `/api/v2/icons` (фіксовані ~2000 популярних, параметр search
  ігнорується → фільтр у браузері). AWTRIX Hub `https://awtrix.de/icons/<id>.gif` — лише з
  Bearer-ключем (без нього 401).
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

## Документація й скриншоти
- README (огляд, галерея, підключення), docs/GUIDE.md (посібник), docs/API.md (REST).
- Скриншоти матриці: `GET /api/v1/display/screen` → PNG «світлодіодами» (PIL з Python ESPHome:
  `/home/linuxbrew/.linuxbrew/Cellar/esphome/*/libexec/bin/python`). Знімати в паузі (~0.7 с після
  `apps/active`), тексти підбирати, щоб влазили (інакше кадр посеред прокручування).
- Скриншоти веб-панелі: Chrome `save_to_disk` + обрізка. **Маскувати SSID і IP** (підміна
  `fetch('/api/state')` у сторінці), вкладку «Система» не знімати (сусідні мережі).
  Демо-дані створювати через API й прибирати після.

## Структура
- `wall-matrix.yaml` — substitutions + пакети.
- `packages/core.yaml` — плата, Wi-Fi, OTA, API (`notify`/`dismiss`/`set_text`).
- `packages/display.yaml` — стрічка, шрифти, іконки MDI, display, гортання 4 екранів.
- `packages/sensors.yaml` — час, погода з HA (`weather_entity`), фоторезистор GPIO4.
- `packages/admin.yaml` — усі налаштування (template-сутності), скрипти `notify_show` / `next_screen` (обгортки над рушієм), блок `matrix_web`.
- `components/matrix_web/` — matrix_web.cpp (HTTP, стан), apps.cpp (рушій), api_v1.cpp (REST), icons.cpp (бібліотека).
- `web/index.html` — веб-панель у стилі AWTRIX NG, vanilla JS.
- `includes/matrix_gfx.h` — цифри 5×14, годинник, дата, температура, текст, іконка, гамма, тест.

## Користувач
Пише російською (відповідати російською), а проєкт веде українською. Любить робити по кроках
і перевіряти на залізі. Зовнішній вигляд важливий: веб-панель має бути «як в AWTRIX».
