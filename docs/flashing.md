# CyBeer — инструкция по прошивке

Плата: **ESP32-C3 SuperMini**. Стек: **ESP-IDF 5.x / 6.x** (не Arduino). Рабочий каталог: `firmware/`.

> **ESP-IDF 6:** зависимости `littlefs`, `cjson`, `mdns`, `led_strip` подтягиваются через Component Manager (`idf_component.yml` в компонентах). Первая сборка скачает их в `firmware/managed_components/`.

## 1. Что нужно на ПК

| Компонент | Назначение |
|-----------|------------|
| [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/) | Сборка, прошивка, монитор порта |
| Python 3 | Идёт в составе ESP-IDF |
| USB-драйвер | Для чипа USB-UART на SuperMini (часто **CH340** или **CP2102**; на Windows — драйвер от производителя) |
| USB-кабель **data** | Только зарядный кабель не подойдёт |

После установки IDF откройте терминал через **ESP-IDF PowerShell** / **ESP-IDF CMD** (Windows) или выполните `export.sh` / `export.ps1`, чтобы в PATH были `idf.py` и `esptool.py`.

## 2. Подключение платы для прошивки

1. Подключите ESP32-C3 SuperMini к ПК по **USB-C**.
2. Определите COM-порт:
   - Windows: Диспетчер устройств → «Порты (COM и LPT)» → например `COM7`.
   - Linux: `ls /dev/ttyUSB*` или `ttyACM*`.
3. При первой прошивке или если порт не виден — удерживайте **BOOT**, нажмите **RESET**, отпустите **BOOT** (режим загрузчика).

Прошивка идёт через встроенный USB-JTAG/UART; отдельный программатор не нужен.

## 3. Первая сборка

```bash
cd firmware
idf.py set-target esp32c3
idf.py build
```

`set-target` достаточно один раз на чистой копии проекта.

Если `set-target` пишет, что каталог `build` «не CMake» — удалите его вручную и повторите:

```powershell
Remove-Item -Recurse -Force build
idf.py set-target esp32c3
``` Целевой чип и размер флеша заданы в `sdkconfig.defaults` (**4 MB**, кастомная таблица `partitions.csv`).

### Разделы флеша

| Раздел | Назначение |
|--------|------------|
| `factory` / `ota_0` / `ota_1` | Приложение (по 0x130000 байт), OTA |
| `littlefs` | Веб-статика (`www`) |
| `nvs` | Wi-Fi, PIN, число LED и т.д. |

При `idf.py build` CMake:

1. Копирует `../frontend/` → `firmware/frontend_dist/www/`
2. Собирает образ **LittleFS** и включает его в прошивку (`FLASH_IN_PROJECT`).

## 4. Прошивка (полный образ)

Подставьте свой порт вместо `COM7` / `/dev/ttyUSB0`:

```bash
cd firmware
idf.py -p COM7 flash
```

Прошиваются **bootloader**, **partition table**, **приложение** и **littlefs**.

### С монитором лога

```bash
idf.py -p COM7 flash monitor
```

Выход из монитора: `Ctrl+]`.

### Только приложение (быстрее, без LittleFS)

Если менялась только логика прошивки, без `frontend/`:

```bash
idf.py -p COM7 app-flash
```

После правок в `frontend/` нужна полная `flash` (или отдельная прошивка раздела `littlefs`, если настроена цель в вашей среде).

## 5. Сброс NVS / полная очистка

| Задача | Команда |
|--------|---------|
| Стереть всю флеш | `idf.py -p COM7 erase-flash` затем `idf.py -p COM7 flash` |
| Только настройки (Wi-Fi, PIN, LED) | `python -m esptool --chip esp32c3 -p COM7 erase-region 0x9000 0x6000` (раздел `nvs` в `partitions.csv`; в ESP-IDF 6 нет `erase-partition`) |

После стирания NVS устройство снова поднимет **AP для настройки Wi-Fi** (см. [README.md](../README.md)).

## 6. Проверка после прошивки

1. В мониторе — логи Wi-Fi, `cybeer`, без паники при старте.
2. Если NVS пустой — SSID вида **`CyBeer-AABBCC`** (три байта MAC, **не** просто `CyBeer`), пароль **нет**, IP **192.168.4.1**, страница **http://192.168.4.1/setup**.
3. После STA — `http://cybeer.local` (mDNS) или IP из `GET /api/status`.
4. Дисплей TM1637 и лента должны реагировать на состояние (см. [wiring.md](wiring.md)).

### Не видно точку доступа Wi-Fi

| Что проверить | Действие |
|---------------|----------|
| Имя сети | Ищите **`CyBeer-`** + 6 hex-символов (например `CyBeer-1A2B3C`). В мониторе: строка `SoftAP SSID:CyBeer-...`. |
| Старые настройки в NVS | Уже сохранён Wi-Fi дома → устройство может быть **только в вашей сети** (без видимого AP). Попробуйте `http://cybeer.local` или IP в роутере. Сброс NVS: `python -m esptool --chip esp32c3 -p COM erase-region 0x9000 0x6000`, затем перезагрузка (прошивку заново не обязательно). |
| USB и ESP32-C3 | На C3 USB и Wi-Fi мешают друг другу: для проверки AP **отключите USB**, питайте от аккумулятора/5 V, подождите 10–15 с и обновите список сетей на телефоне (только **2.4 GHz**). |
| Режим настройки | Без сохранённого Wi-Fi лента мигает **синим** (режим provisioning). |
| Полный сброс | `idf.py -p COM erase-flash` → `idf.py -p COM flash` (стирает и NVS, и прошивку). |

## 7. Обновление без USB (OTA)

После первой USB-прошивки образ можно обновлять по сети (нужен **админ-PIN**):

- `POST /api/admin/ota/url` — загрузка по HTTPS
- `POST /api/admin/ota/upload` — бинарник до **0x130000** байт

Подробности: [firmware/README.md](../firmware/README.md).

## 8. Типичные проблемы

| Симптом | Что проверить |
|---------|----------------|
| `Failed to connect` | BOOT+RESET, другой USB-порт/кабель, драйвер CH340/CP2102 |
| Нет `idf.py` | Запуск из ESP-IDF shell или `export.ps1` |
| Сайт не открывается | Прошит ли `littlefs` (полная `flash`, не только `app-flash` после смены frontend) |
| Старая вёрстка в браузере | Жёсткое обновление страницы; пересоберите и прошейте снова |

## 9. Сборка unit-тестов (опционально)

```bash
cd firmware/test
idf.py set-target esp32c3
idf.py build
```

Тесты периферии на железе не требуются для обычной прошивки устройства.
