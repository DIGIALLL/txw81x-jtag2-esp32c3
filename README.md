# txw-prog — JTAG2 programmer for Taixin TXW81x cameras on an ESP32-C3

*English summary:* a DIY two-wire C-SKY JTAG2 (CKHAD) programmer for Taixin **TXW817/TXW81x**
Wi-Fi mini cameras, made from an **ESP32-C3 SuperMini** (~$2) instead of an official CK-Link.
It dumps and writes the camera's SPI flash, which lets you replace the cloud firmware
with [OpenTXW81X](https://github.com/openshwprojects/OpenTXW81X) (OpenBeken port) and get
a local RTSP stream in Home Assistant. The protocol was reverse-engineered from CK-Link Lite
firmware and T-Head DebugServer; see [`re/NOTES.md`](re/NOTES.md). Docs are in Russian.

---

Китайские мини-камеры на **Taixin TXW817-810** (ядро C-SKY CK803) из коробки отправляют видео
и пароль Wi-Fi на облачный сервер. Этот проект позволяет без покупки CK-Link:

1. подключиться к отладочному интерфейсу камеры двумя проводами;
2. сохранить полный дамп родной прошивки (1 МБ);
3. записать [OpenTXW81X](https://github.com/openshwprojects/OpenTXW81X) с поблочной проверкой;
4. получить локальный RTSP и [завести камеру в Home Assistant](docs/home-assistant.md).

Проверено на двух платах, обе TXW817, флеш 1 МБ (ID `0xCD1460CD`):

| Плата | Сенсор | Видео после OpenTXW81X | Ноги C3 (HCK/HDA) |
|---|---|---|---|
| чёрная `STD-X5-817-V02` | HI704 | 640×480 MJPEG, ~8 к/с | GPIO7 / GPIO5 |
| синяя круглая `IOT_V1.2 D250609` | SP0828 | 240×320 MJPEG, ~13 к/с, звук 8 кГц | GPIO5 / GPIO7, `--wdt-off` |

## Состав

| Путь | Что это |
|---|---|
| `fw/txw-prog/` | прошивка программатора для ESP32-C3 (Arduino): `txw-prog.ino` + `jtag2.h` |
| `tools/txwprog.py` | утилита для ПК: `id`, `dump`, `write` |
| `tools/*.py` | диагностические скрипты (буфер ОЗУ, стирание, тест записи) |
| `re/NOTES.md` | разбор протокола JTAG2/HAD, регистры, найденные грабли |
| `re/*.py`, `re/thead/*.py` | скрипты, которыми разбирались прошивка CK-Link и DLL DebugServer |
| `docs/wiring.md` | проводка для чёрной и синей плат |
| `docs/home-assistant.md` | RTSP → go2rtc → Home Assistant |

## Что нужно

- ESP32-C3 SuperMini (или любой ESP32-C3 с нативным USB);
- тонкие провода, паяльник;
- Python 3.10+, пакеты `pyserial`, `pyelftools`;
- `arduino-cli` с ядром esp32;
- алгоритм флеша `Taixin_TXW81X_FLASH_ALGORITHM.elf` из
  [openshwprojects/FlashTools](https://github.com/openshwprojects/FlashTools), его нужно положить в
  `re/flashalgo/txw81x_algo.elf` (в репозиторий не входит);
- образ [OpenTXW81X](https://github.com/openshwprojects/OpenTXW81X/releases).

## Сборка прошивки программатора

```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc fw/txw-prog
arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p COMx fw/txw-prog
```

## Использование

Проводка описана в [`docs/wiring.md`](docs/wiring.md). Затем:

```
set TXW_PORT=COMx
python tools/txwprog.py id                                   # HID и ID флеша
python tools/txwprog.py dump stock.bin --size 0x100000        # дамп 1 МБ, ~5 мин
python tools/txwprog.py write OpenTXW81X_x.y.z.bin --yes      # стирание + запись + проверка
# синяя плата: добавить --hck 5 --hda 7 --wdt-off
```

- Чтение идёт со скоростью около 3,7 КБ/с, запись образа занимает около 5,5 минуты. Каждый
  4-КБ блок перечитывается и сверяется.
- Если запись прервалась, её можно продолжить: `--skip-erase --start 0x<смещение>`.
- **Сначала сделайте дамп и сохраните его в двух местах.** В дампе есть ваш пароль Wi-Fi:
  не выкладывайте его.
- После чтения или записи ядро остаётся остановленным, и новая прошивка не запустится, пока не передёрнуть
  питание камеры. Перед этим полезно отпустить линии (`!jrel`): иначе камера может подпитываться через
  ноги программатора и не сбросится.

### Команды прошивки программатора (через монитор порта)

`!jpins <hck> <hda> <rst>` · `!jinit` · `!hid` · `!jbrute [мс]` · `!jprobe` · `!dbg` · `!csr` ·
`!regr/!regw` · `!mrd <addr> [n]` · `!mwr` · `!go` · `!call` · `!wblk/!rblk` · `!wblkf/!rblkf` ·
`!jspeed` · `!log 0|1` · `!baud` · `!auto` · `!rx <gpio>`

После открытия порта C3 перезагружается, поэтому каждый раз сначала нужны `!jpins 7 5 3` и `!jinit`.

## Главные грабли (подробно в `re/NOTES.md`)

- Перед запуском кода ставить HCR = 0x4000 (снять DR), иначе ядро идёт пошагово.
- Бит 7 CSR нужен, чтобы `bkpt` останавливал ядро. После запуска биты 7/8 залипают: для обычных
  ld/st брать `CSR & 0xfe7f`.
- Не взводить HCR.DR, если CPU уже в отладке: запрос запоминается и срывает следующий запуск.
- Флеш по XIP-адресу 0x18000000 при чтении данных отдаёт 0xFF. Читать нужно через алгоритм Taixin в ОЗУ.
- `flashErase` только подаёт команду. Дальше нужно ждать `spiflash_wait_busy` и проверять, что блок стал FF.
- Если родная прошивка включила сторожевой таймер (синяя плата), нужен флаг `--wdt-off`. Без него чип сбрасывается во время загрузки алгоритма.

## Статус

- [x] Чёрная `STD-X5-817-V02`: дамп, прошивка OpenTXW81X, RTSP, Home Assistant.
- [x] Синяя круглая `IOT_V1.2 D250609` (SP0828): дамп (с `--wdt-off`), прошивка OpenTXW81X, RTSP, Home Assistant.

## Благодарности

- [openshwprojects/OpenTXW81X](https://github.com/openshwprojects/OpenTXW81X) и
  [FlashTools](https://github.com/openshwprojects/FlashTools): прошивка и алгоритм флеша.
- Проект cklink-lite-fw-convertor: прошивка CK-Link Lite для STM32F103, по ней восстановлен протокол.

Проприетарные файлы (прошивки CK-Link, DLL T-Head, даташиты, дампы камер) в репозиторий
не входят.

## Лицензия

MIT — см. [LICENSE](LICENSE). Используете на свой риск: неудачная запись может оставить
камеру без прошивки, если у вас нет дампа.
