# CyBeer — Design Specification

**Date:** 2026-05-17  
**Status:** Approved  
**Platform:** ESP32-C3 SuperMini, ESP-IDF 5.x

## 1. Purpose

Секундомер для чемпионата по скоростному питью пива в 3D-печатном корпусе (платформа + основание). Устройство фиксирует время заезда по положению бутылки на платформе (линейный свитч), показывает результат на TM1637, подсвечивает корпус WS2812B, ведёт историю заездов и турнирную сетку через веб-интерфейс на самом ESP.

## 2. Hardware

| Компонент | Роль |
|-----------|------|
| ESP32-C3 SuperMini | MCU, Wi-Fi, веб-сервер, OTA |
| TM1637 (4 цифры, двоеточие по центру) | Отображение `SS:cc` во время заезда и в подготовке |
| Линейный свитч (клавиатурный) | Бутылка **на платформе** → контакт **замкнут** (pressed / LOW) |
| WS2812B | Контурная подсветка корпуса |
| Аккумулятор + USB-C | Питание; уровень через ADC |
| 3D-корпус | Платформа (свитч) + основание (электроника, лента) |

**Число LED:** не фиксируется при сборке; задаётся в веб-UI (NVS), с прошивным верхним пределом `LED_COUNT_MAX` (64). При изменении — пересоздание буфера ленты после сохранения настроек (кратковременная перезагрузка допустима).

## 3. GPIO Assignment (ESP32-C3 SuperMini)

| Сигнал | GPIO | Примечание |
|--------|------|------------|
| `SWITCH` | **9** | Input, pull-up; LOW = бутылка на платформе |
| `TM1637_CLK` | **6** | Open-drain / push-pull per library |
| `TM1637_DIO` | **7** | Bidirectional data |
| `WS2812_DATA` | **8** | RMT LED strip driver (IDF `led_strip`) |
| `BATTERY_ADC` | **0** | ADC1_CH0; делитель 2:1 (100k/100k) от аккумулятора → ~0–4.2 V на пине |

**Не использовать для периферии:** GPIO18/19 (USB-JTAG на многих C3), GPIO2 (strapping на части плат — не занят).

Питание ленты: 5 V от Step-up/BMS, общая GND с ESP; резистор 330 Ω на DATA; конденсатор 100–1000 µF у ленты. Подробности — в README.

## 4. Timer & Switch Logic

### 4.1 Debounce (v1 — вариант B)

- Дребезг: **40 ms**
- Минимум «бутылка на платформе» перед переходом в подготовку: **200 ms**
- Минимум «бутылка снята» перед стартом таймера: **150 ms**

Пороги — `#define` / Kconfig; при необходимости ужесточаем до явных состояний с отменой коротких заездов.

### 4.2 State Machine

```
                    ┌──────────────────────────────────────┐
                    │  PREP (подготовка к старту)          │
                    │  Бутылка НА платформе, таймер стоит  │
                    │  TM1637: 00:00                       │
                    └───────────────┬──────────────────────┘
                                    │ сняли бутылку (stable)
                                    ▼
                    ┌──────────────────────────────────────┐
                    │  RUNNING                             │
                    │  TM1637: SS:cc live                  │
                    │  WebSocket: live time                │
                    └───────────────┬──────────────────────┘
                                    │ поставили бутылку (stable)
                                    ▼
                    ┌──────────────────────────────────────┐
                    │  FINISHED                            │
                    │  Фиксация duration (µs), unclaimed   │
                    │  run в storage, LED «claim pending»  │
                    │  TM1637: финальное SS:cc             │
                    └───────────────┬──────────────────────┘
                                    │ сняли бутылку (замена/уборка)
                                    ▼
                    ┌──────────────────────────────────────┐
                    │  READY                               │
                    │  Бутылка СНЯТА после финиша          │
                    │  (= готовность к следующему циклу)   │
                    │  TM1637: последний результат заезда │
                    └───────────────┬──────────────────────┘
                                    │ поставили бутылку (новая/та же)
                                    ▼
                              PREP (00:00)
```

**Ключевые отличия от ранней версии:**

- После финиша **снятие** бутылки (в т.ч. для замены) → **READY**, не «ошибка» и не новый старт.
- **Возврат** бутылки на платформу → **PREP**, на дисплее **`00:00`** (без чередования 5 с / 5 с).
- Старт таймера только из **PREP** при устойчивом снятии.
- В **READY** на TM1637 показывается **последний зафиксированный** результат (до возврата бутылки).

### 4.3 Time Precision

| Место | Точность |
|-------|----------|
| Внутренний счёт | `esp_timer_get_time()` → микросекунды |
| Веб / API / экспорт | Полное значение (µs или ms с дробной частью) |
| TM1637 | Секунды + сотые, формат `SS:cc` с двоеточием |

## 5. Software Stack

**ESP-IDF 5.x** (не Arduino).

| Подсистема | Реализация |
|------------|------------|
| Wi-Fi | `esp_wifi`; режим **AP+STA** |
| STA provisioning | NVS credentials; при отсутствии — AP + captive portal `/setup` (DNS hijack + HTTP redirect) |
| HTTP | `esp_http_server` |
| WebSocket | `httpd_ws_*` — live timer, события состояния |
| OTA | `esp_https_ota` или `esp_ota_ops` + раздел `ota_1`; опционально upload через admin |
| LED | `led_strip` (RMT) + эффекты в отдельном модуле |
| TM1637 | Компонент/драйвер bit-bang или RMT |
| Storage | **NVS** (настройки, PIN hash) + **LittleFS** (JSON: participants, runs, tournaments) |
| Battery | `esp_adc` + калибровка; процент в API |

Структура проекта (IDF):

```
firmware/
  main/
  components/
    cybeer_timer/
    cybeer_switch/
    cybeer_display/
    cybeer_led/
    cybeer_storage/
    cybeer_wifi/
    cybeer_web/
  partitions.csv      # factory + ota_0 + ota_1 + littlefs
spiffs_or_littlefs/   # веб-ассеты при сборке
README.md
docs/wiring.md        # дублирует ключевое из README при необходимости
```

## 6. Wi-Fi

- **STA:** подключение к сети площадки; mDNS **`cybeer.local`**
- **AP:** `CyBeer-XXXX` (суффикс из MAC); IP **`192.168.4.1`**
- **AP+STA** одновременно: прямой доступ к устройству + доступ из LAN площадки
- **Первый запуск / нет NVS Wi-Fi:** только AP + `/setup` (captive portal)
- **Повторная настройка:** `/setup` или кнопка «Забыть Wi-Fi» в admin (сброс STA credentials)

## 7. Data Model

### 7.1 Participant

```json
{ "id": "uuid", "name": "string", "createdAt": "iso8601" }
```

### 7.2 Run

```json
{
  "id": "uuid",
  "participantId": "uuid | null",
  "durationUs": 12345678,
  "finishedAt": "iso8601",
  "claimed": false,
  "tournamentMatchId": "uuid | null"
}
```

- После **FINISHED** создаётся run с `claimed: false`.
- Пользователь в вебе вводит/выбирает имя → `claimed: true`, `participantId` set.
- Хранится **полная история**; у участника считаются: count, best, worst, average, last.

### 7.3 Tournament

```json
{
  "id": "uuid",
  "name": "string",
  "format": "single_elimination",
  "status": "draft | active | completed",
  "participants": ["participantId"],
  "matches": [
    {
      "id": "uuid",
      "round": 1,
      "slot": 0,
      "playerA": "participantId | null",
      "playerB": "participantId | null",
      "runIdA": "uuid | null",
      "runIdB": "uuid | null",
      "winnerId": "participantId | null"
    }
  ]
}
```

**Турнирный режим (v1):**

- Admin создаёт турнир, добавляет участников, генерирует сетку **single elimination** (bye при нечётном числе).
- Активный матч отображается на главной; после заезда run привязывается к слоту матча (A или B).
- Победитель матча — **меньшее время** (быстрее выпил); admin может переопределить.
- Свободные заезды (без турнира) остаются доступны параллельно.

## 8. Web Application

### 8.1 Pages

| Path | Описание |
|------|----------|
| `/` | Лидерборд, последние заезды, активный турнир/матч |
| `/claim` | Привязка имени к последнему unclaimed run |
| `/player/:id` | Статистика участника |
| `/tournament` | Сетка, управление (admin) |
| `/admin` | PIN: CRUD, export, OTA trigger, Wi-Fi reset, LED count |
| `/setup` | Wi-Fi provisioning (captive) |

### 8.2 WebSocket (`/ws`)

События (JSON):

- `state` — PREP | READY | RUNNING | FINISHED
- `timer` — `{ "elapsedUs": N }` (10–20 Hz в RUNNING)
- `runFinished` — `{ "runId", "durationUs" }`
- `leaderboardUpdate` — после claim / admin edit

### 8.3 REST API (выборочно)

- `GET /api/status` — state, battery%, wifi, ledCount, firmware version
- `GET /api/runs`, `GET /api/participants`, `GET /api/tournaments/active`
- `POST /api/runs/:id/claim` — `{ "name" }` или `{ "participantId" }`
- `POST /api/admin/*` — с заголовком `X-Admin-Pin`
- `GET /api/export?format=json|csv`
- `PUT /api/settings` — `{ "ledCount": N }` (admin)

### 8.4 Admin (PIN в NVS, bcrypt или salted hash)

- Удаление/правка run и participant
- Ручное добавление run
- Полный сброс истории
- Экспорт JSON/CSV
- Создание/ведение турнира
- Запуск OTA (URL или upload)
- Сброс Wi-Fi credentials
- Настройка `ledCount`, яркости ambient

### 8.5 Battery

- ADC → напряжение → процент (кривая Li-ion, настраиваемые пороги в Kconfig).
- Веб: иконка батареи в шапке, обновление по poll/WebSocket (`battery` event).

## 9. LED Effects

| Режим | Условие |
|-------|---------|
| **Ambient** | PREP, Wi-Fi OK, нет активных алертов |
| **Armed** | PREP (бутылка на месте, 00:00) — чуть ярче/другой hue |
| **Running** | RUNNING — бегущий огонь |
| **Finished flash** | FINISHED — короткая вспышка |
| **Claim pending** | unclaimed run exists — медленное мигание |
| **Podium** | после claim: топ-3 или личный рекорд — анимация → ambient |
| **Wi-Fi setup** | AP provisioning — синий пульс |
| **OTA** | прогресс — индикация по периметру (опционально) |

Цвета/яркость: defaults в Kconfig; тонкая настройка — admin (v1: ledCount + brightness достаточно).

## 10. OTA

- Partition table: `factory` + `ota_0` + `ota_1` + `littlefs`
- Admin: указать HTTPS URL прошивки **или** upload `.bin` (размер лимит в Kconfig)
- После успешной OTA — перезагрузка; версия в `/api/status`
- README: как собрать и выкатить OTA-бинарник

## 11. README Scope

1. Схема подключения (таблица GPIO, делитель батареи, лента, TM1637, свитч)
2. Требования: ESP-IDF, Python, драйвер USB
3. Сборка и прошивка: `idf.py build flash`, прошивка LittleFS
4. Первый запуск Wi-Fi и mDNS
5. Использование на чемпионате (PREP → снять → пить → поставить → claim в вебе)
6. OTA для организатора
7. Настройка числа LED в admin

## 12. Out of Scope (v1)

- Пьезо-зуммер
- QR на корпусе (можно добавить в README как ссылку без печати)
- Telegram/webhook export
- Double elimination / round-robin (только single elimination)

## 13. Testing Strategy

- Unit: debounce/state transitions (host tests или ESP test app)
- Integration: mock switch GPIO, проверка переходов PREP→RUNNING→FINISHED→READY→PREP
- Web: API claim flow, tournament bracket progression
- Manual: реальная бутылка, WS timer latency, OTA rollback path

## 14. Decisions Log

| Решение | Выбор |
|---------|--------|
| Wi-Fi | AP + STA + captive setup |
| Framework | ESP-IDF 5.x |
| Leaderboard | Полная история + статистика по участнику |
| Claim | После заезда в вебе (имя/выбор) |
| Post-finish flow | Сняли → READY; поставили → PREP (00:00) |
| Debounce | B (настраиваемые пороги) |
| Admin | PIN + export + CRUD + tournament + OTA + ledCount |
| LED count | Web UI → NVS, max 64 |
| Extras in v1 | WebSocket live, OTA, battery icon, tournament bracket |
