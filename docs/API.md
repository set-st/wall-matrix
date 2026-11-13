# REST API Wall Matrix

Маршрути, методи й формат помилок — як в **AWTRIX NG** (`/api/v1/...`). Авторизації немає.
Помилки: `{"error":{"code":"notFound","message":"…"}}`. Успіх: `{"ok":true}`.

Числа можна передавати й рядками (`"20000"`) — так інколи роблять шаблони Home Assistant.
POST без тіла ESPHome відхиляє з кодом 411 — у curl додавайте `-d ''`.

## Застосунки

| Запит | Що робить |
|---|---|
| `GET /api/v1/apps` | черга: `[{name, enabled, inLoop, slot, present, origin, active, durationMs, title?, text?, …}]` |
| `PUT /api/v1/apps/order` `{"order": [...], "disabled": [...]}` | порядок і вимкнені застосунки |
| `PUT /api/v1/apps/active` `{"name": "Text"}` | показати застосунок зараз |
| `POST /api/v1/apps/next`, `POST /api/v1/apps/previous` | наступний / попередній |
| `POST` / `PUT /api/v1/apps/<ім'я>` | створити / оновити pushed-застосунок |
| `DELETE /api/v1/apps/<ім'я>` | видалити pushed-застосунок або інформер |

`origin`: `builtin` (`Time`, `Date`, `Weather`, `Text`), `informer` (`inf_N`), `pushed`.

### Pushed-застосунок

До 16 штук, живуть у пам'яті до перезавантаження (місце в черзі зберігається). `POST /api/v1/apps/co2`:

```json
{
  "text": "CO2 640",
  "icon": "co2",
  "color": "#00FF80",
  "durationMs": 8000,
  "lifetimeMs": 600000,
  "progress": 64,
  "progressColor": "#00A0FF",
  "show": true
}
```

| Поле | Опис |
|---|---|
| `text` | текст; `\n` — другий рядок; підстановки `{time}` `{date}` `{day}` `{temp}` |
| `icon` | ім'я іконки з бібліотеки |
| `color` | `"#RRGGBB"`, число `0xRRGGBB` або `[r, g, b]` |
| `durationMs` / `duration` | скільки показувати (мс / с); типово — як «Текст» |
| `lifetimeMs` / `lifetime` | видалити, якщо не оновлювали стільки часу; 0 — ніколи |
| `progress`, `progressColor` | смужка внизу 0..100 |
| `show` | одразу показати |

Ім'я: 1–32 символи `A-Z a-z 0-9 _ -`, не вбудоване й не `inf_*`.

## Інформери

| Запит | Що робить |
|---|---|
| `GET /api/v1/informers` | `[{id, title, single, durationMs, lines: [{entity, attr, fmt, icon, color, unit, preview}×2]}]` |
| `POST /api/v1/informers` | створити; без `lines` — шаблон. Відповідь `{"ok":true,"id":"inf_N"}` |
| `PUT /api/v1/informers/<id>` | змінити (тіло як у GET, без `id` і `preview`) |
| `DELETE /api/v1/informers/<id>` | видалити |

`preview` — поточний текст рядка (лише читання). До 12 інформерів, зберігаються у флеші.

## Сповіщення

| Запит | Що робить |
|---|---|
| `POST /api/v1/notifications` | показати поверх застосунків |
| `DELETE /api/v1/notifications/active` | прибрати поточне |
| `DELETE /api/v1/notifications/<ім'я>` | прибрати іменоване (з черги теж) |

```json
{ "text": "Пральна машина закінчила", "icon": "wash", "color": "#FFD200",
  "durationMs": 5000, "hold": false, "stack": true, "name": "wash" }
```

`stack: false` — замінити чергу; `hold: true` — до явного `DELETE`; однакове `name` — оновити на місці;
`frameColor` — колір рамки для сповіщення без іконки.

## Дисплей

| Запит | Що робить |
|---|---|
| `GET /api/v1/display` | `{power, brightness (0–255), brightnessPct, autoBrightness}` |
| `PATCH /api/v1/display` | змінити будь-що з переліченого |
| `GET /api/v1/display/screen` | `{width, height, pixels: [0xRRGGBB, …]}` — поточний кадр у видимих кольорах |

## Іконки

| Запит | Що робить |
|---|---|
| `GET /api/v1/icons` | `{icons: [{name, width, height, frames, bytes}], usedBytes, totalBytes, maxIcons}` |
| `GET /api/v1/icons/<ім'я>` | іконка у двійковому форматі |
| `PUT` / `POST /api/v1/icons/<ім'я>` | зберегти (тіло `application/octet-stream`) |
| `DELETE /api/v1/icons/<ім'я>` | видалити |

Формат: `'W' 'M' N W H 0`, далі N кадрів: затримка u16 LE (мс) + W×H×3 байти RGB.
Розмір 8×8 або 16×16, N ≤ 16. Картинки й GIF у цей формат перетворює веб-панель.

## Підключення матриці

| Запит | Що робить |
|---|---|
| `GET /api/v1/hardware` | `{active: {mode, pin, serpentine}, saved: {…}, statusPin, leds, pins: [дозволені GPIO]}` |
| `PUT /api/v1/hardware` `{"mode": "chain"\|"direct", "pin": 5, "serpentine": false, "reboot": true}` | зберегти (діє після перезавантаження; `reboot: true` — одразу перезавантажити) |

`chain` — матриця після вбудованого світлодіода плати (`statusPin`), `direct` — на `pin`.

## Wi-Fi

| Запит | Що робить |
|---|---|
| `GET /api/v1/wifi` | `{connected, ssid, rssi, ip, apActive, networks: [{ssid, rssi, lock}]}` |
| `POST /api/v1/wifi` `{"ssid": "…", "password": "…"}` | зберегти мережу й перепідключитися |

## Home Assistant: `rest_command`

```yaml
rest_command:
  matrix_app:
    url: "http://192.168.1.50/api/v1/apps/{{ name }}"
    method: post
    content_type: "application/json"
    payload: >-
      {"text": "{{ text }}", "icon": "{{ icon | default('') }}",
       "color": "{{ color | default('#FFFFFF') }}", "lifetimeMs": 900000}
  matrix_notify:
    url: "http://192.168.1.50/api/v1/notifications"
    method: post
    content_type: "application/json"
    payload: '{"text": "{{ text }}", "icon": "{{ icon | default('''') }}", "stack": false}'
```

```yaml
# Автоматизація: CO₂ як pushed-застосунок, оновлення щохвилини (lifetime 15 хв)
- alias: "Wall Matrix: CO2"
  triggers:
    - trigger: time_pattern
      minutes: "/1"
  actions:
    - action: rest_command.matrix_app
      data:
        name: co2
        text: "{{ states('sensor.co2') | int }} ppm"
        icon: co2
        color: "{{ '#FF4040' if states('sensor.co2') | int > 1000 else '#00FF80' }}"
```

Краще вказувати IP панелі: `.local` резолвиться не в кожній інсталяції HA.

## Відмінності від AWTRIX NG

- Немає скриптів на пристрої (Berry), звуку, ефектів/палітр, оверлеїв і файлового менеджера.
- Іконки — у власному двійковому форматі, а не GIF/JPG-файли.
- `POST /api/v1/apps/<ім'я>` для pushed-застосунків і `/api/v1/informers` — наші розширення.
- `GET /api/v1/system` навмисно немає: він віддає мережеві налаштування.

## Службові маршрути веб-панелі

`GET /api/state`, `GET /api/live`, `POST /api/set`, `POST /api/press` — для `web/index.html`, можуть
змінюватися.
