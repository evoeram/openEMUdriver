# E-MU USB (CA0189) — протокол управления

Источники:
1. **Официальный открытый драйвер E-MU «CA0189 USB Audio Driver» для macOS** — выпущен E-MU Systems/Creative Technology Ltd в 2008 под **LGPLv2.1+** (копия: `ref/EMU-driver/src/EMUUSBAudio`). Это первоисточник wire-протокола.
2. Дизасемблирование Windows-драйвера 1.30.07 (`emusba10.sys`, `emasio.dll`, `EmuUsbAudioCP.exe`) — перекрёстная проверка.

## Устройства

| PID (VID 041E Creative) | Кристалл | Кодовое имя | Продукт |
|---|---|---|---|
| 0x3F02 | EM8740 | MicroPod | E-MU 0202 USB |
| 0x3F04 | EM8761 | HulaPod | E-MU 0404 USB |
| 0x3F0A | EM8730 | MicroPre | Tracker Pre USB |
| 0x3F0C (в поздних INF) | — | — | E-MU 0204 USB |

Аудио-часть — стандартный **USB Audio Class 1.0** (поток, alt settings, Feature Unit с Volume/Mute,
S/PDIF как PCM-каналы). Кастом E-MU живёт в **Extension Units** (XU) на AudioControl-интерфейсе.

## Extension Units

XU-дескрипторы различаются по полю `wExtensionCode`:

| wExtensionCode | Назначение | Селекторы управления (CS) |
|---|---|---|
| `0xE301` | Clock Rate | 2 = поддержка частот (GET), 3 = выбор частоты (GET/SET) |
| `0xE302` | Clock Source | 2 = источник (1 байт: 0 = internal, 1 = S/PDIF ext) |
| `0xE303` | Digital I/O Status | 2 = цифровая частота (4 байта, big-endian), 3 = lock, 4 = SRC, 5 = формат вывода (0=none,1=spdif,2=aes) |
| `0xE304` | Device Options | 2 = analog pad, 3 = soft limit (0=off, 1=on) |
| `0xE305` | Direct Monitoring | 2 = стерео-структура (20 байт), 3 = вкл/выкл |
| `0xE306` | Metering | GET-данные пиков/счётчиков |

Unit ID ищется **динамически** сканированием дескрипторов (не хардкодится).

### Формат запроса (UAC1 class-specific, recipient = Interface)

```
bmRequestType = 0x21 (OUT, SET_CUR) / 0xA1 (IN, GET_CUR)
bRequest      = 0x01 (SET_CUR/GET_CUR)
wValue        = (CS << 8) | channelNumber(0)
wIndex        = (unitID << 8) | interfaceNumber(AudioControl)
wLength       = 1 (стандартно) или 4 (digital sample rate, big-endian)
```

Частоты (enum eSampleRate): 0=44100, 1=48000, 2=88200, 3=96000, 4=176400, 5=192000.
Обратная операция: XU `0xE301` CS=3, 4-байтовая частота (big-endian).

### Feature Unit (громкость)

Стандартный UAC1: MUTE(1 байт)/VOLUME(SInt16, 1/256 дБ, диапазон по GET_MIN/GET_MAX;
референс E-MU: max +12 дБ, min −127 дБ, master channel index 1).

## Direct Monitoring (20-байтная структура)

В LGPL-драйвере помечено «currently no support» — точная раскладка полей восстанавливается
из `EmuUsbAudioCP.exe`/`emasio.dll` (см. RE_NOTES.md). Параметры на канал:
gain (0..0x7FFFFFFF), pan (0=лево..0x7FFFFFFF=право), mute, вход→выход (kDirectMonInput/Output).

## Latency-таблицы оригинального драйвера (emusba10.dat, ×100 нс)

| Устройство | Ур.0 @48k | Ур.1 @48k |
|---|---|---|
| 0202 USB | 16459 (~1.65 мс) | 46667 (~4.7 мс) |
| 0404 USB | 18125 (~1.8 мс) | 48334 (~4.8 мс) |

ASIO-буферы: `ArrayOffset=7` в emasio.dat (массив размеров буфера в emasio.dll).
