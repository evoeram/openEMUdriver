# RE-заметки: E-MU USB 1.30.07 (`EMUU_PCAppDrvFw_L6_1_30_07.exe`)

SHA-256: `42f65ca4ddc1d089725333f60b1fb5bf24a01fffa472ae57e79331f32d9236e4`

## Состав пакета (после распаковки, `C:\dev\EMU\EMUU_PCAppDrvFw_L6_1_30_07`)

- `Audio/Drivers/driver/` — `emusba10.sys` (amd64/i386), `emasio.dll`/`emasio.drv`,
  `emaudsv.exe`, `emcoinst.dll`, `emaud.inf`, `emaud.cat`, `emasio.dat`, `emusba10.dat`
- `Audio/UsbAudio/Bin.cab` — панель **EmuUsbAudioCP.exe** (Qt4) + EmuUsbAudioCPServices.dll
- `Audio/UFU/` — прошивка `EMU_USB_AUDIO_2007-11-19_v8.04.pkg` + Qt-updater
- `Audio/Setup/` — InstallShield (engine32.cab, setup.inx)

## Устройства (emaud.inf)

VID_041E (Creative): PID_3F02 = EM8740 «MicroPod» (0202 USB), PID_3F04 = EM8761 «HulaPod»
(0404 USB), PID_3F0A = EM8730 «MicroPre» (Tracker Pre). Композитное устройство:
AudioControl + AudioStreaming out + AudioStreaming in (+MIDI).

## Архитектура оригинала (по импортам и декомпиляции, Ghidra 11.3.2)

- `emusba10.sys` — форк микродрайвера **usbaudio** Microsoft (пути сборки
  `huladrv\driver\usbaud10\adapter\hula`), AVStream/KS, сам USB-хост
  (`USBD_CreateConfigurationRequestEx`), фильтры Audio/Midi/Control
  (KSCATEGORY_DEVICECONTROL «E-MU Control»).
- `emasio.dll` — COM-сервер ASIO: `KsCreatePin` (ksuser) — поток через KS-пины ядра;
  управление — `DeviceIoControl` с приватным IOCTL **0x2f0003** на handle устройства.
- Панель (`EmuUsbAudioCPServices.dll`, класс `CHulaServices`/`CreateHulaServices`) —
  тот же IOCTL 0x2f0003 (= IOCTL_KS_PROPERTY); вход — структура KSPROPERTY
  { GUID **{F6D7C04D-C5C3-4181-9ED7-504ABE0CDDFF}**, Id, Flags } (+ payload),
  флаги: 1=GET, 2=SET, 0x10000001/0x10000002 = KSPROPERTY_TYPE_TOPOLOGY|GET/SET
  (узловой уровень KSP_NODE, наборы {26BB0000|XUcode-PID-VID-…}).
  Полный реверс: **docs/PANEL_IOCTL.md** (таблицы Id 0-7/0x20/0x21/0x10000-10002,
  payload-раскладки, URB/EP0 слой, {26BBExx}→XU мост, INI-ключи).
- `emaudsv.exe` — служба-арбитр (автозапуск), DeviceIoControl + PnP-нотификации.

## Данные

- `emasio.dat` — INI: имена каналов по PID; `[PID.Audio.BufferSize] ArrayOffset=7`;
  `Sync=3`; AppHacks для Live 4/5 (EnableResyncRequest=0).
- `emusba10.dat` — INI: таблицы латентности ×100 нс по уровням 0/1
  (0202 @48k: 16459 ≈ 1.65 мс; 0404 @48k: 18125 ≈ 1.8 мс; уровень 1 ≈ 4.7-4.8 мс).
- В `emasio.dll` найдены наборы размеров {8,16,24,32} (kernel-очередь, не ASIO-фреймы)
  и чтение `ArrayOffset` через GetPrivateProfileIntA из `%SystemRoot%\emasio.dat`.
  ASIO-размеры буфера отчётливо power-of-2 (64..4096) в наших терминах.

## Протокол XU (перекрёстно подтверждён)

Официальный LGPL-исходник E-MU «CA0189 USB Audio Driver» (macOS) ↔ дизас Windows:
см. docs/PROTOCOL.md. Unit ID'ы ищутся динамически по wExtensionCode 0xE301..0xE306
(subtype 0x08 EXTENSION_UNIT, Table A-5 UAC1).

## Выводы для openEMUdriver

1. Поток — чистый UAC1 (PCM, 24-bit/3 байта, async feedback EP). Нужен только
   WinUSB-движок, kernel-часть не требуется.
2. Управление — XU через EP0 (SET_CUR/GET_CUR на AudioControl-интерфейсе).
3. Панельный IOCTL 0x2f0003 воспроизводить не нужно (приватный путь оригинала).
4. Direct Monitoring (XU 0xE305, CS=2, 20 байт) и Metering (0xE306) — точная
   раскладка полей не восстановлена (в Mac-версии не реализована; в панели
   0404 Direct Monitor отсутствует — аппаратная ручка). TODO v2: перехват USB
   трассировкой оригинального стека.

## Инструменты

Ghidra 11.3.2 + JDK 21 (headless, `DumpAll.java` — выгрузка декомпиляции),
pefile/capstone (Python), 7-Zip (распаковка), WinLibs GCC 16.2 (сборка).

## Живое железо (0404 USB, прошивка 8.04, ноя 2026)

Дескрипторы: **все аудио-интерфейсы объявлены vendor-class (0xFF)**, но несут
стандартную UAC1 class-specific структуру. usbccgp собирает их в один узел MI_00,
WinUSB-хендл которого покрывает всю конфигурацию (associated interfaces 1..3).
Форматы: OUT/IN 2ch 44.1–192 кГц + 4ch 44.1–96 кГц (альтернативные сеттинги),
24 бит/3 байта, EP0 OUT iso + feedback EP 0x81 (4 байта, 16.16), capture EP 0x82
(explicit implicit-feedback), AC interrupt EP 0x83 (2 байта).

### Ключевые находки

1. **GET_CUR (bmRequestType 0xA1) прошивка не реализует вовсе** — сталл на любых
   class/vendor IN-запросах (даже GET_STATUS). SET_CUR (0x21) работает.
   Это объясняет известные «аномалии Linux» (snd-usb-audio создаёт контролы
   только после удачного GET — частота не переключается). Статусы устройство
   уведомляет через interrupt EP 0x83 (пакет c0 0e = изменение unit 0x0E).
2. **WinUsb_WriteIsochPipeAsap/ReadIsochPipeAsap непригодны**: цепочка Asap-трансферов
   мгновенно завершается ERROR_INVALID_PARAMETER (требование кратности
   max-bytes-per-interval делает невозможными точные 1мс-пакеты асинхронного аудио).
   Решение: **WinUsb_WriteIsochPipe / WinUsb_ReadIsochPipe с явными номерами
   кадров** — детерминированная сетка 1 мс, ровно 1000.0 трансферов/с, 0 ошибок.
3. Isoch-буфер регистрировать **после** установки alt-сеттинга: регистрация
   привязывается к пайпу текущего альта, а alt 0 — zero-bandwidth (87 на всех
   трансферах при обратном порядке).
4. Feedback EP отдаёт 4 байта (16.16), а не 3 (10.14).

### Результаты asio-host-test (0404 USB, буфер 256)

| Частота | Каденция факт | Цель |
|---|---|---|
| 44100 | 5.807 мс | 5.805 |
| 48000 | 5.333 мс | 5.333 |
| 96000 | 2.674 мс | 2.667 |
| 192000 | 1.337 мс | 1.333 |


## WASAPI-режим: эксперимент с инбоксным usbaudio.sys (ноя 2026)

INF «compat mode» привязывает всё устройство к usbaudio.sys — драйвер загружается
(PnP OK, svc=usbaudio), но **не создаёт ни одного audio endpoint'а**: он ищет
интерфейсы класса 0x01, а CA0189 объявляет их vendor-class (0xFF) — фирмварь
специально скрывает UAC-структуру от инбоксного стека (чтобы продавать свой
драйвер). Именно поэтому E-MU сделала форк usbaudio (emusba10.sys).

Вывод: **WASAPI на этой прошивке недостижим без собственного kernel-драйвера**
(порт emusba10.sys). Механизм переключения режимов (oed-mode.ps1) оставлен:
в будущем в ASIO-режиме останется прямой WinUSB, в kernel-режиме — WASAPI+DirectKS.

Подводные камни переключателя режимов (реестр/store):
- pnputil /enum-drivers локализован — парсить по имени INF как целому токену
  (openemu.inf — подстрока openemu-compat.inf!);
- после переустановки пакета AddReg может не примениться к уже установленному
  устройству — DeviceInterfaceGUIDs придётся прописывать вручную или удалять
  devnode (pnputil /remove-device) с последующим физическим replug.
