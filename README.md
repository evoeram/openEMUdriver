# openEMUdriver

Открытый драйвер для E-MU USB-интерфейсов (**0202 USB, 0404 USB, Tracker Pre USB, 0204 USB**)
на чипе Creative CA0189. Замена штатному пакету E-MU 1.30.07: быстрый, стабильный,
с **настоящим ASIO** — пользовательский COM-ASIO-драйвер, который стримит изохронный
UAC1-поток **напрямую через WinUSB**, без WDM/KS-прослойки и без kernel-драйверов.

Лицензия: **LGPL-2.1-or-later** (протокол восстановлен по официально выпущенному E-MU
открытому драйверу «CA0189 USB Audio Driver» под LGPL; код Windows-драйвера написан с нуля).

## Два поколения

| | v1 — ASIO/WinUSB | v2 — kernel-драйвер |
|---|---|---|
| Что это | COM ASIO 2 DLL + изохронный движок в пользовательском режиме | `EmuUSBAudio.sys` на PortCLS: стандартный WASAPI-эндпоинт Windows |
| Кому | ASIO-хостам (DAW), монопольно | всем Windows-приложениям (WASAPI) + DirectKS ASIO (план M6) |
| Статус | **работает** (протестировано на живой 0404, прошивка 8.04) | **render работает: звук на железе** (2.0.0.70, 2026-11-07); capture и остальные частоты — M5 |

Драйверы **взаимоисключающие**: они претендуют на один devnode. `kinstall.ps1` при
установке v2 снимает v1-пакеты, `-Uninstall` возвращает WinUSB.

## Состав репозитория

| Путь | Что это |
|---|---|
| `src/`, `panel/`, `inf/openemu.inf` | v1: исходники ASIO-драйвера, панели, привязка к WinUSB |
| `bin/` | сборка v1: `openEMUdriver.asio`, `openEMUPanel.exe`, `emu-dump.exe`, `asio-host-test.exe` |
| `driver/emuusbaudio/Emu/` | v2: исходники kernel-драйвера (PnP, USB, дескрипторы, ISO-движок, WaveRT, IOCTL) |
| `driver/tests/` + `driver/utest.sh` | юнит-тесты двойных исходников (хост-режим + proof-компиляция kernel-флагами) |
| `driver/tools/` | `emu-probe`, `stats_dump`, `pump_probe` — проверка живого устройства |
| `driver/kbuild.sh`, `driver/ksign.ps1`, `driver/kinstall.ps1`, `driver/khw*.ps1`, `driver/ksoak.ps1` | сборка, подпись, установка, hardware-прогоны, soak под Verifier |
| `docs/` | реверс и планы: см. «Документы» ниже |
| `tools/` | `wasapi-test.exe` (эндпоинты/тон), `ks-probe.c`, `get-ref.sh` |
| `VERIFIER_RECOVERY.txt` | выход из boot-loop под Driver Verifier (без потери данных) |

## Как это работает (v1)

```
DAW ──IASIO──► openEMUdriver.asio ──WinUSB iso (1 мс/пакет, кольцевой буфер,
                                     feedback-клок)──► E-MU 0404 USB (UAC1)
```

- поток и управление — стандартный USB Audio Class 1.0 + vendor Extension Units
  `0xE301..0xE306` (частота, клок, S/PDIF, soft limit, direct monitor, метры);
- 1 isochronous-передача на USB-кадр => точные размеры пакетов на **всех**
  частотах, включая 44.1/88.2 кГц;
- буферы ASIO: 64…4096 фреймов (по умолчанию 256), глубина очереди USB — `QueueMs` (по умолчанию 4 мс);
- underrun → удержание последнего фрейма + счётчик xruns.

## Установка v1 (Windows 10/11 x64)

1. **Привязать устройство к WinUSB**: Диспетчер устройств → «E-MU 0404» (или раздел с
   жёлтым восклицательным знаком) → Обновить драйвер → «Найти на этом компьютере» →
   «Выбрать из списка» → «Установить с диска» → `inf/openemu.inf`
   (либо `pnputil /add-driver inf\openemu.inf /install` от администратора).
   Отключить устройство и включить обратно (или переткнуть USB).
2. **Зарегистрировать ASIO**: `regsvr32 bin\openEMUdriver.asio` (от администратора).
   В реестре появится `HKLM\SOFTWARE\ASIO\openEMUdriver`.
3. Проверка: `bin\emu-dump.exe` (должен показать устройство и открыть его),
   затем `bin\asio-host-test.exe` — 2 секунды потока с тоном 440 Гц и замером каденции.
4. Панель: `bin\openEMUPanel.exe`.

Настройки (опционально), `HKLM\SOFTWARE\ASIO\openEMUdriver` (REG_DWORD):
`PreferredBufferSize` (фреймы, 64–4096), `QueueMs` (глубина USB-очереди, 2–16).

## v2 — kernel-драйвер (WASAPI)

```
WASAPI ──KS/WaveRT──► EmuUSBAudio.sys (PortCLS)
                          ├─ WaveRT-ринг: OS пишет пакеты, движок упаковывает int24→packed24
                          ├─ ISO-движок (модель вендора emusba10): 1 URB/мс = 2×144 Б,
                          │  8 слотов, якорь StartFrame = шина+3, feedback EP 0x81 (+32 кадра)
                          └─ control plane: rate/clock/S/PDIF/soft limit через EP0 (XU 0xE301..)
```

Движок — обратная сборка штатного `emusba10.sys` (трейс 65 255 URB: дельта StartFrame
+1/URB, feedback-дельта ровно 32, очередь 8, ресинк из completion — `docs/ISOCH_SCHED.md`).
Поверх вендорской модели добавлены ограничители, которых у вендора не было:
**bounded fail-stop** (первый URB-уровневый отказ записывается и останавливает поток,
а не кормит сам себя), **таймер-отложенный feedback re-arm** (никаких перезарядок
внутри собственного completion-DPC), **потолок опережения курсора** и останов через
`IoCancelIrp`. Полная история итераций с разбором каждого падения — `docs/ISOCH_PUMP.md`.

### Этапы (docs/V2_PLAN.md)

| Этап | Что | Статус |
|---|---|---|
| M0. База и воспроизводимость | тулчейн ищется сам, подпись отдельным скриптом | ✅ 2026-11-04 |
| M1. PnP-каркас | INF/StartDevice/Remove; soak 50 циклов под Verifier — ни одного BSOD | ✅ 2026-11-05 |
| M2. USB + управление | парсер дескрипторов, кэш состояния, полный SET-путь (rate/clock/S/PDIF/soft limit) на живой 0404 | ✅ 2026-11-05 |
| M3. Kernel ISO без PortCLS | тестовый IOCTL: URB-кольцо, StartFrame, статистика | ✅ A-gate закрыт на железе; движок переписан в модель вендора |
| M4. Минимальный WaveRT | 48 кГц, 2 канала, 24-in-32, render; USB-позиция, notifications | ✅ **render: ЗВУК на железе** (2.0.0.70); capture стримит, полнота IN-пакетов — на разбор (M5) |
| M5. Форматы и full duplex | все частоты 0404, 4 канала, feedback→rate, xruns | план |
| M6. DirectKS ASIO | ASIO поверх KS без WinUSB | план |

### Уроки железа (почему это было сложно)

Каждый вердикт получен проводом, не теорией; детали — `docs/ISOCH_PUMP.md` §9–11.

1. `USBD_AssignUrbToIoStackLocation` **не** пишет `IOCTL_INTERNAL_USB_SUBMIT_URB` — без него
   сабмиты молча не доходят до контроллера.
2. Перезарядка feedback-URB из собственного completion-DPC на USB3-стеке циклится
   **внутри одного DPC** → `DPC_WATCHDOG_VIOLATION`. Re-arm — только через таймер.
3. `USBD_PIPE_INFORMATION.Interval` — это **raw bInterval** дескриптора. Устройство
   показывает каждую скорость дважды: альт с сервис-интервалом 1 мс и альт с 500 мкс
   (MPS = кадры×6+10 — отсюда «загадочные» +10). Вендорская модель 2×144 Б/мс легальна
   только на 500-мкс альте — на 1-мс альтe capture получает babble/CRC, render —
   ~49% `ISO_TD_ERROR` и тишина. Выбор альта = выбор расписания провода.

### Проверено на железе (2.0.0.70)

- сессия воспроизведения 59 588 URB (~60 с) — `fail=0, perr=0`;
- `pump_probe` (стресс-проба, ронявшая систему): 4236 URB, ноль ошибок, чистый стоп;
- `AltSelR/C = 0x104/0x204`, `TestPipe = Interval 3, MPS 154`, `ClampFires=0`,
  `FailStage=0`, `Underruns=0` — все диагностические маркеры в норме.

## Сборка

```bash
./build.sh                                # v1 (MinGW-w64, Windows SDK не нужен)
cd driver
./utest.sh                                # v2 юнит-тесты: 6/6 (хост-режим + kernel-компиляция /WX)
./kbuild.sh emuusbaudio/build/emuusbaudio.sys \
    emuusbaudio/Emu/emu_driver.cpp emuusbaudio/Emu/emu_pnp.cpp \
    emuusbaudio/Emu/emu_disp.cpp emuusbaudio/Emu/emu_usb.cpp \
    emuusbaudio/Emu/emu_desc.cpp emuusbaudio/Emu/emu_ctrl.cpp \
    emuusbaudio/Emu/emu_state.cpp emuusbaudio/Emu/emu_ioctl.cpp \
    emuusbaudio/Emu/emu_isoch.cpp emuusbaudio/Emu/emu_audio.cpp \
    emuusbaudio/Emu/emu_wave.cpp emuusbaudio/Emu/emu_newdelete.cpp   # v2 ядро (MSVC + WDK 10.0.26100)
```

Компиляторы ищутся сами (переменные `OED_*` — только для переопределения).
Полная инструкция для чистой машины, включая установку тулчейна и подпись:
**[docs/BUILD.md](docs/BUILD.md)**.

## Установка v2 (осторожно: это kernel-драйвер)

Требуется один раз: тестовая подпись (`bcdedit /set testsigning on` + перезагрузка)
и сертификат (`driver/ksign.ps1 -CreateCert -InstallCert`). Скрипты сами boot-конфигурацию не трогают.

```powershell
cd driver
.\kinstall.ps1            # собрать, подписать, поставить; старый образ выгрузится после перезагрузки
.\kinstall.ps1 -Uninstall # назад на v1 WinUSB
```

После установки **нужна перезагрузка**: загруженный образ kernel-драйвера не
сменяется restart-device'ом. Для hardware-прогонов: `khw.ps1` (probe),
`khw-restart.ps1` (перезапуск девноды + probe), `ksoak.ps1` (50 циклов).
Если включали Driver Verifier и получили boot-loop — `VERIFIER_RECOVERY.txt`
(без потери данных). Стрейминг-пробы: `driver\tools\pump_probe.exe` и
`stats_dump.exe` от администратора (control-устройство admin-only).

## Диагностика v2

- `emu-probe.exe` — capabilities, состояние, счётчики start/stop;
- `stats_dump.exe` — счётчики ISO-движка по направлениям (submits/completes/errors);
- `iso-diag.exe` — по-URB история провода (план/факт по каждому пакету,
  статусы IRP/URB/пакета; P1-разбор capture, см. `docs/ISOCH_PUMP.md` §12);
- `pump_probe.exe` — самозавершающаяся стрим-проба (render+capture, 8×500 мс срезов);
- breadcrumbs — именованные значения в
  `HKLM\SYSTEM\CurrentControlSet\Services\EmuUSBAudio\Parameters` (пишутся драйвером:
  `AltSelR/C`, `TestPipe0/1`, `CalFramesPerMs`, `FailStage/FailStatus`, `ClampFires`,
  `Fq*`, `Pump*Exit`, `FillReject*`...), читаются `reg query ... /s`;
- DebugView-трейс: `khw-trace.ps1` (фильтр IHVDRIVER).

## Статус v1 (протестировано на реальной 0404 USB, прошивка 8.04)

- ✅ Поток 44.1 / 48 / 96 / **192 кГц**, 2 и 4 канала, 24 бит — точная ASIO-каденция
  (256-фреймовый буфер: 5.333 мс @48k и т.д.), 0 ошибок трансферов.
- ✅ Выбор частоты, клока, S/PDIF-формата, soft limit (XU SET).
- ⚠️ Прошивка 8.04 **не реализует GET-запросы** (статусы только через interrupt-EP) —
  панель показывает выставленные значения; чтение состояния — v2.
- ⚠️ Feedback-эндпоинт пока не опрашивается (номинальные размеры пакетов; дрейф
  компенсируется кольцевым буфером) — включение опроса запланировано: параллельные
  iso-чтения feedback-пайпа ломают планирование OUT-пайпа (см. RE_NOTES).
- **Direct Monitor** (XU 0xE305) и **метры** (0xE306): на 0404 с этой прошивкой
  юниты отсутствуют в дескрипторах; раскладка 20-байтной структуры — v2.

## Ограничения (честно)

- v1: Windows-аудио (WASAPI) на устройстве отсутствует — устройство отдаётся ASIO
  монопольно (как и у оригинального драйвера).
- v2: пока только render 48 кГц / 2 канала / 24-in-32 (формат M4); capture стримит,
  но полнота IN-пакетов уточняется (M5); feedback в позицию не заведён (M5);
  подпись тестовая — для дистрибутива нужна attestation-подпись (Partner Center).

## Документы

| Файл | Содержание |
|---|---|
| `docs/V2_PLAN.md` | план v2, архитектура, этапы M0–M6 и итоги каждого |
| `docs/BUILD.md` | сборка с нуля (MinGW + MSVC/WDK), подпись, установка, soak |
| `docs/ISOCH_SCHED.md` | реверс планировщика emusba10: трейс, URB-формы, константы |
| `docs/ISOCH_PUMP.md` | модель насоса, итерации 1–4: каждый вердикт провода и падения |
| `docs/PROTOCOL.md` | протокол XU чипа CA0189 |
| `docs/PANEL_IOCTL.md` | IOCTL-контракт панели (v2 control plane) |
| `docs/RE_NOTES.md` | результаты реверса оригинального пакета 1.30.07 |
| `VERIFIER_RECOVERY.txt` | выход из boot-loop под Driver Verifier |

## Удаление

v1: Device Manager → устройство → Удалить (с галкой «удалить драйвер») → переткнуть USB;
`regsvr32 /u bin\openEMUdriver.asio`; удалить `HKLM\SOFTWARE\ASIO\openEMUdriver`.

v2: `cd driver && .\kinstall.ps1 -Uninstall` (снимает пакет, возвращает v1 WinUSB,
переткнуть USB). Если v2-пакет не снимается штатно — удалить пакет `openemu-ks.inf`
через `pnputil /enum-drivers` + `pnputil /delete-driver oemXX.inf` и переткнуть устройство.
