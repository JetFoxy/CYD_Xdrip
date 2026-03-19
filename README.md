# CYDDrip

Монитор глюкозы на базе ESP32-2432S028 ("Cheap Yellow Display").

Получает данные с CGM через Android-приложение CYDDrip по BLE и отображает их на встроенном 2.8" дисплее.

## Возможности

- Текущий уровень глюкозы с цветовой индикацией (зелёный / жёлтый / красный)
- Стрелка тренда
- Мини-график за последние 3 часа (36 точек × 5 мин)
- Delta между последними двумя показаниями
- IoB, CoB, последний болюс, данные помпы
- Текущее местное время
- Звуковые тревоги при выходе за пороги (через динамик CN1)
- Регулировка яркости кнопкой
- Сохранение истории в NVS (переживает перезагрузку)
- Конфигурация через `CYD.INI` на SD-карте
- WiFi + прямое подключение к Nightscout (альтернатива BLE)
- NTP синхронизация времени при наличии WiFi

## Железо

| Компонент | Описание |
|-----------|----------|
| Плата | ESP32-2432S028 (CYD) |
| Дисплей | ILI9341 2.8" 320×240 (встроен) |
| Динамик | 8Ω 0.5W, JST 1.25mm 2-pin → CN1 |
| Кнопка яркости | Тактовая 6×6mm → P3 GPIO 27 + GND |
| Кнопка сброса тревоги | Тактовая 6×6mm → P3 GPIO 22 + GND |
| SD-карта | FAT32, microSD → встроенный слот |

Подробная распиновка и схемы подключения: [docs/HARDWARE.md](docs/HARDWARE.md)

## Сборка

Проект на [PlatformIO](https://platformio.org/).

```bash
pio run --target upload
```

Все настройки TFT_eSPI заданы через `build_flags` в `platformio.ini` — файл `User_Setup.h` не нужен.

## Конфигурация

Создай файл `CYD.INI` в корне SD-карты:

```ini
[config]
show_mgdl=0          ; 0 = mmol/L, 1 = mg/dL
utc_offset_min=180   ; UTC+3 (Москва), резервный если нет NTP
bg_low=3.9
bg_warn_low=4.5
bg_warn_high=9.0
bg_high=10.0
brightness=2         ; 0=тускло, 1=средне, 2=ярко
; blepassword=secret

[wifi]
ssid=MyNetwork
password=MyPassword

[nightscout]
url=https://mysite.herokuapp.com
; token=mytoken-xxxxxxxx
```

Если SD-карты нет — используются значения по умолчанию.

## BLE

Устройство представляется как `M5Stack` для совместимости с CYDDrip.
Изначально основано на M5Stack Nightscout

Протокол: WatchDrip CYD (opcodes 0x09 / 0x0A / 0x20 / 0x21).
Полное описание пакетов: [docs/HARDWARE.md](docs/HARDWARE.md#ble-protocol-watchdrip-cyd)

## Тревоги

| Условие | Сигнал |
|---------|--------|
| BG < `bg_low` или > `bg_high` | 3 коротких писка (срочная) |
| BG < `bg_warn_low` или > `bg_warn_high` | 1 писк (предупреждение) |

Повтор каждые 5 минут. Кнопка сброса (GPIO 22) глушит тревогу до выхода из зоны.

## Лицензия

GPL-3.0. Основано на M5Stack Nightscout monitor by Johan Degraeve.
