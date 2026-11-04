# openEMUdriver v2 — kernel-драйвер (WASAPI + ASIO)

Статус и карта работ. Продолжение отсюда = новая сессия начинается с раздела
«Что реально есть сейчас» и таблицы этапов.

Пересмотрено 2026-11-04 по итогам рецензии первой версии плана. Главные
изменения: отказ от ручной USB-FDO, обязательный feedback-движок, позиция
WaveRT от USB-completions, этапы вместо оценки «в строках».

## Архитектура

```
sysaudio/WASAPI ──KS──► ┌───────────────────────────┐
                        │ EmuUSBAudio.sys (PortCLS) │
DAW ──DirectKS ASIO───► │  WaveRT render + capture  │──iso URB──► 0404 USB
openEMU Panel ──IOCTL──►│  XU control (EP0)         │──ctrl URB──► (SET работает,
                        └───────────────────────────┘              GET — нет)
```

* WASAPI/MME создаётся автоматически: PortCLS-фильтры (KSCATEGORY_AUDIO/RENDER/
  CAPTURE) + wdmaud-регистрация (как в INF оригинала).
* ASIO открывает KS-пины эксклюзивно (KsCreatePin, как оригинальный emasio.dll).
* Контролы — через приватный IOCTL-интерфейс драйвера (панель и ASIO); драйвер
  шлёт EP0 SET_CUR по протоколу из docs/PROTOCOL.md.

### PnP-схема (исправлено)

Портативный USB-FDO вручную создавать **нельзя**: `PcAddAdapterDevice` сам
создаёт FDO и присоединяет его к стеку; вторая ручная FDO на том же PDO ломает
PnP/ power-архитектуру. Приватные IOCTL обрабатываются на том же FDO (свой
handler для своих кодов, остальное — `PcDispatchIrp`); control-PDO на первом
этапе не нужен.

```
DriverEntry
  └─ PcInitializeAdapterDriver

AddDevice(USB PDO)
  └─ PcAddAdapterDevice
       └─ PortCLS FDO внутри реального USB-стека

StartDevice(PortCLS FDO)
  ├─ USB control plane (см. ниже) — полностью откатываемый
  ├─ разбор дескрипторов → capabilities устройства
  ├─ общий device context
  ├─ регистрация приватного control interface
  └─ создание WaveRT render/capture фильтров
```

IAdapterCommon реализовать без `CHw`/`hw.h` (файл лежит в
`driver/emuusbaudio/Utilities/hw.h`, но зависимость от сэмпловой обвязки
убираем): Init/GetDeviceObject/SetWaveServiceGroup/Mixer* (локальное
состояние)/InstallSubdevice/ConnectTopologies/InstallEndpointFilters/
RemoveEndpointFilters/GetFilters/SetIdlePowerManagement/Cleanup,
WriteEtwEvent — пустой.

## Этап control plane внутри StartDevice

При любой ошибке — освобождение ресурсов в обратном порядке.

1. Получить configuration descriptor.
2. Сформировать `USBD_INTERFACE_LIST_ENTRY` по всем интерфейсам.
3. Выбрать конфигурацию через `USBD_CreateConfigurationRequestEx`.
4. Сохранить USBD handle, pipe handles, параметры всех endpoint'ов.
5. Разобрать UAC1-дескрипторы, **несмотря на vendor class 0xFF** (firmware CA0189
   прячет UAC-структуру от инбоксного usbaudio.sys — см. RE_NOTES).
6. Построить capabilities: модель и PID; номера AC/AS интерфейсов;
   alt setting → частота/каналы/битность; data/feedback/interrupt endpoint'ы;
   XU unit ID; `MaximumPacketSize` и `Interval`.
7. Проверить EP0 SET_CUR.
8. Зарегистрировать IOCTL-интерфейс.
9. Только после этого создавать аудио-endpoint'ы.

В первой версии kernel-INF — только PID 0404, чтобы не подхватывать
непроверенные 0202/0204/Tracker Pre.

## USB-транспорт

Планирование опирается не на предположение «1 пакет = 1 мс», а на реальные
данные пайпа: скорость USB-устройства (full/high), `USBD_PIPE_INFORMATION.Interval`,
`MaximumPacketSize`, polling period и дескрипторы конкретного alt setting.

* Текущий кадр — `URB_FUNCTION_GET_CURRENT_FRAME_NUMBER`; `USBD_QueryBusTime`
  не основной источник. `StartFrame` задаётся заранее с запасом; поздние кадры и
  выход за допустимый диапазон — отдельные обработчики ошибок.
* Первый вертикальный срез повторяет v1: один пакет в URB, 6–8 URB в полёте.
  После стабилизации — 4–8 пакетов в одном URB с независимыми offsets и
  отдельной проверкой статуса каждого пакета.
* Обязательно проверять: статус URB, статус каждого
  `USBD_ISO_PACKET_DESCRIPTOR`, фактическую длину IN-пакета,
  `USBD_STATUS_BAD_START_FRAME`, поздние/пропущенные пакеты, короткие и
  нулевые capture-пакеты.
* Iso-пайпы: OUT 0x01, feedback IN 0x81 (4 байта, 16.16 — не 10.14), capture IN
  0x82, AC interrupt IN 0x83 (2 байта). 24 бит/3 байта на канале;
  2ch 44.1–192 кГц, 4ch 44.1–96 кГц (192 кГц только 2ch — связку 4ch/192k
  корректно отклонять).
* Alt setting: alt 0 — zero-bandwidth; буферы/пайпы привязывать **после**
  `URB_SELECT_INTERFACE` (в WinUSB обратный порядок давал статус 87).

## Feedback — обязателен, а не опционален

Кольцевой буфер не компенсирует постоянную разницу часов, он лишь откладывает
переполнение/underrun. Отдельный feedback engine:

```
EP 0x81 → 4 байта 16.16 → проверка диапазона → фильтр выбросов/сглаживание
        → целевая samples-per-USB-interval → 64-bit fixed-point accumulator
        → размеры следующих OUT-пакетов
```

Никаких `double` в потоковом kernel-пути. Ожидаемо: 44.1 кГц → устойчивый
паттерн 44/45, 88.2 → 88/89 и т. д.

Параллельный OUT + feedback в WinUSB не списывать заранее на «особенности
интервала» (в v1 поток feedback отключён, см. комментарий в
[src/emu_stream.cpp:86](file:///C:/dev/openEMUdriver/src/emu_stream.cpp#L86)):
снять USBPcap/ETW-трассу, проверить дескрипторы, pipe handles, длины буферов,
StartFrame. Номинальный режим — диагностический fallback, не release-режим.

## Позиция WaveRT — от USB completions

В SimpleAudioSample позиция считается по QPC и программному таймеру; для
реального устройства заменить на:

* линейный 64-битный счётчик фактически переданных/принятых аудиофреймов;
* `GetPosition` — offset внутри циклического WaveRT-буфера, как требует интерфейс;
* QPC — только для presentation timestamp и ASIO timestamp, не для «аппаратной»
  позиции;
* `GetPositionRegister`/`GetClockRegister` — неподдерживаемые (регистров нет);
* notification-события — по USB completion/аудиопериоду, а не постоянным 1 мс
  таймером сэмпла.

При потерянном capture-пакете — вставить тишину ожидаемой длины и продвинуть
позицию по времени шины, увеличив счётчик ошибок; иначе позиция встанет и
клиент зависнет.

## Модель владения устройством

* Одна глобальная частота на render и capture; вторая direction открывается
  только с той же частотой.
* Смена частоты и alt setting — только когда оба потока остановлены.
* Render-only / capture-only / full duplex — отдельные явные состояния.
* ASIO использует те же KS-пины эксклюзивно; WASAPI + ASIO одновременно не
  гарантируются.
* Панель работает независимо от того, кто владеет аудиопотоком.
* Внешний S/PDIF clock без подтверждённого lock — поток не запускается, ASIO
  получает `ASE_NoClock`.

Состояния потока:

```
STOPPED → PREPARED → RUNNING → PAUSED
    ↑         ↓          ↓
    └── STOPPING ←───────┘

из любого состояния → SURPRISE_REMOVED / DEVICE_GONE
```

Все URB, MDL, буферы и объекты потока освобождаются только после rundown
завершённых и отменённых запросов.

## IOCTL-контракт (описать до реализации панели)

Из user mode не передавать произвольные `unitId`, `controlSelector` и длины —
только закрытый набор операций:

```
GET_CAPS · GET_STATE · SET_SAMPLE_RATE · SET_CLOCK_SOURCE
SET_SPDIF_FORMAT · SET_SOFT_LIMIT · GET_STREAM_STATS
```

Каждая структура содержит: `Size`, `Version`, фиксированное выравнивание для
x64, `SupportedMask` и `ValidMask`, `StateSequence`, `LastOperationStatus`,
диагностические счётчики.

Кэш состояния называть «последнее подтверждённо отправленное состояние» (не
фактическое состояние прошивки — GET_CUR firmware не реализует вовсе). После
replug, power resume или surprise removal оно становится *unknown* до повторной
инициализации. Все SET-операции сериализуются на `PASSIVE_LEVEL`.

## Что реально есть сейчас

| Часть | Состояние |
| --- | --- |
| v1 ASIO/WinUSB | Зрелая часть: ASIO COM DLL, панель, диагностика, INF, packed-24 streaming, явное кадровое планирование. На 0404 (fw 8.04) заявлены 44.1/48/96/192 кГц, 2/4 канала |
| v2 kernel | Каркас: SimpleAudioSample, smoke.c, ручной kbuild.sh, план. Последний коммит — WIP |
| USB в kernel | Отсутствует: выбор конфигурации, URB-пул, EP0, interrupt EP, feedback, iso-движок |
| PortCLS | Практически исходный сэмпл: stereo/16-bit/48 kHz, ToneGenerator, программный таймер, эмуляция DMA-позиции |
| INF / ASIO / панель v2 | Не реализованы |
| Воспроизводимость | M0 выполнен: `build.sh` и `driver/kbuild.sh` сами находят компилятор, WDK и SDK (локальные пути убраны), подпись вынесена в `driver/ksign.ps1`, `ref/` поднимается по запросу `tools/get-ref.sh`, дамп дескрипторов сохраняется `emu-dump -o`. Инструкция и фактические проверки — `docs/BUILD.md`. CI и автотестов по-прежнему нет |

## Этапы и критерии готовности

Не по строкам кода, а по проверяемым этапам.

| Этап | Содержание | Критерий готовности |
| --- | --- | --- |
| M0. База и воспроизводимость | ✅ сделано 2026-11-04 (см. «Итог M0»): v1 tag'ом; убрать локальные пути из сборки (поиск тулчейна вместо MSBuild-проекта — `.vcxproj` из сэмпла ещё не подключены); отдельный signing-скрипт; восстановить или удалить ссылки на `ref/`; сохранять descriptor dumps | Чистая машина собирает v1 и smoke/v2 по инструкции — **выполнено на новой машине без WDK** |
| M1. PnP-каркас 0404 | `PcAddAdapterDevice`, device context, remove lock, test-INF только 0404, корректные start/stop/remove/surprise-remove. Без аудио | 50 циклов disable/enable и remove/replug под Driver Verifier без утечек и BSOD |
| M2. USB + управление | Выбор конфигурации, parser capabilities, EP0 SET_CUR, кэш состояния, версионные IOCTL, безопасная регистрация device interface | Панель отдельно меняет rate/clock/S/PDIF/soft limit; WinUSB одновременно не открывается |
| M3. Kernel ISO без PortCLS | Временный test-only IOCTL для 48 kHz stereo render/capture: URB-кольцо, StartFrame, статистика ошибок | Синус на OUT и loopback capture работают независимо от PortCLS; тестовый интерфейс удалён из release |
| M4. Минимальный WaveRT | Только 48 kHz, 2 канала, 24-in-32. Сначала render, затем capture. Реальные буферы вместо ToneGenerator, USB-позиция, notifications | WASAPI render и capture + wasapi-test проходят; generator/savedata/timing emulation удалены |
| M5. Форматы и full duplex | Все частоты 0404, затем 4 канала 44.1–96 кГц, feedback, external clock, обработка xruns | Все разрешённые сочетания работают; запрещённые (4ch/192k) корректно отклоняются |
| M6. DirectKS ASIO | Бэкенд поверх существующей ASIO COM-оболочки: поиск фильтра, создание пинов, allocator framing, asynchronous I/O, KSSTATE, sample position | asio-host-test работает без WinUSB; 64–4096 frames, render/capture/full duplex |
| M7. Панель и установка | Панель на IOCTL, режимы winusb и kernel, удаление сломанного/experimental wasapi-режима из обычного переключателя, INF-регистрация wdmaud | Повторяемая установка/удаление/переключение режимов без ручной правки реестра |
| M8. Стабилизация | Sleep/resume, surprise removal во время DAW, Verifier, low-memory simulation, soak, PDB и диагностические counters | Критерии раздела «Тесты» выполняются; затем attestation signing и релиз |
| M9. Остальные устройства | Descriptor dumps и матрица capabilities для 0202/Tracker Pre/0204, включение PID по одному | Каждая модель проверена на реальном железе |

Direct Monitor, metering и MIDI — **вне V2.0** (раскладка полей XU 0xE305/0xE306
не восстановлена; при whole-device binding честно указать «MIDI не
поддерживается»).

## Итог M0 (2026-11-04)

Критерий проверялся именно на машине, где тулчейна не было вообще (ни MSVC, ни
MinGW, ни WDK, ни `ref/`) — это и есть «чистая машина» из формулировки этапа.

Выполнено и проверено:

* v1: `./build.sh` → 5 целей (`openEMUdriver.asio`, `emu-dump`, `asio-host-test`,
  `wasapi-test`, `openEMUPanel`); `emu-dump` и `wasapi-test` запускаются.
  `wasapi-test` раньше вообще не собирался скриптом, хотя бинарь лежал в `bin/`.
* v2: `driver/kbuild.sh emuusbaudio/build/smoke.sys emuusbaudio/smoke.c` →
  `BUILT: … (MSVC 14.44.35207, kits C:\Program Files (x86)\Windows Kits\10,
  SDK 10.0.26100.0, WDF 1.35)`.
* Подпись: `driver/ksign.ps1` — отдельный шаг, `signtool sign /fd SHA256` с
  меткой времени проходит; `verify /pa` на self-signed сертификате честно
  ругается на непродоволченный цепочкой доверия корень (лечится
  `-InstallCert`, требует админ-прав).
* `ref/`: `tools/get-ref.sh samples` поднимает sparse-клон
  Windows-driver-samples (pin `f4c1ff842b6a0001152f44cb61788b216a1c7627`),
  `emu-driver` объясняет, где LGPL-исходник и почему его может не быть.
* Дампы: `emu-dump -o docs/descriptors` пишет raw configuration descriptor
  (hex) + разобранную таблицу форматов/XU-юнитов — фикстура для парсера M2.
  Закоммиченных дампов нет: снимается только с живого железа.

Открытые вопросы, которые тянем в M1:

* Полное дерево сэмпла **компилируется** (14 объектов, 0 ошибок), но не
  линкуется: 9 неразрешённых символов — user-mode `/GS`-поддержка из
  `LIBCMT` (`gs_support`/`gs_report` тянут `__imp_QueryPerformanceCounter`,
  `__imp_TerminateProcess` и т. п.). Решение — либо действительно выкинуть
  сэмпловую обвязку (`hw.cpp`, `savedata.cpp`, `ToneGenerator.cpp`), как
  требует план, либо явно разбираться с CRT (`/NODEFAULTLIB`,
  `BufferOverflowK.lib`). `.vcxproj`/MSBuild из сэмпла не подключены:
  пока `kbuild.sh` покрывает требование воспроизводимости, MSBuild-проект —
  отдельная задача.
* CI и автотестов нет (M0 их и не обещал).

Эмпирика по установкам: NuGet `Microsoft.Windows.SDK.CPP.x64` содержит только
библиотеки (`c/ucrt/x64`, `c/um/x64`) — заголовки `ucrt/um/shared` даёт SDK MSI;
NuGet `Microsoft.Windows.WDK.x64` (105 МБ) — это `km`-заголовки, `km`-либы и
`wdf/kmdf` до 1.35. Символы `WdfFunctions_01035`/`WdfDriverGlobals` даёт
`wdfdriverentry.lib`.

## Тесты (обязательные)

1. Parser: реальные дескрипторы всех моделей, malformed lengths, лишние
   endpoint'ы, превышение размеров массивов.
2. Packet scheduling: 44.1/48/88.2/96/176.4/192 кГц, wrap StartFrame, поздняя
   отправка, предельный packet size.
3. 24↔32 conversion: ноль, максимум, минимум, −1, знаковое расширение,
   выравнивание valid bits.
4. Ring/positions: wrap буфера, длинные счётчики, underrun, capture overflow,
   пропущенные пакеты.
5. Жизненный цикл: удаление USB во время RUN, sleep/resume, остановка DAW,
   аварийное завершение ASIO-клиента.
6. Конкуренция: WASAPI держит пин → ASIO пытается открыться; панель меняет
   настройки во время потока; full duplex с разными частотами.
7. Soak: ≥8 часов full duplex 48 kHz и по часу на остальных частотах, без
   необъяснённого накопления drift, контролируемое число xruns.
8. Driver Verifier: Special Pool, Force IRQL Checking, PnP/Power checks,
   отдельно Low Resources Simulation. Первые запуски — на тестовой машине с
   kernel debugger и crash dump, не на рабочей системе.

## Что починить в v1 перед заморозкой

Иначе переходит в v2:

* `EmuStream::start()` возвращает успех до того, как потоковые потоки реально
  инициализировали alt setting и iso-буферы ([src/emu_stream.cpp:18](file:///C:/dev/openEMUdriver/src/emu_stream.cpp#L18));
* `volatile` для `m_ringRd`/`m_ringWr`/`m_run` — не корректная синхронизация;
* проверять completion status ISO-трансферов, а не только событие завершения;
* в `stop()` нельзя игнорировать таймаут `WaitForSingleObject` перед
  освобождением буферов потока;
* startup/shutdown render и capture должны иметь единого владельца ошибки, а не
  независимо менять общий `m_run`.

## Практический следующий шаг

Не переписывать сразу весь `adapter.cpp`: сделать M0–M2 и получить надёжный
PortCLS FDO с USB configuration + EP0 control; затем отдельно доказать
ISO-движок на 48 kHz (M3) и только после этого подключать WaveRT (M4). Это
резко сужает пространство ошибок по сравнению с одновременной отладкой новых
USB-, PnP- и аудиостека.

## Инструмент: проверенные факты (не менять без причины)

* Тулчейн ядра доказан и переставляется: MSVC 14.44.35207 (BuildTools 17.14.44) +
  WDK MSI 10.1.26100.6584 + Windows SDK 10.0.26100.0, всё из winget
  (`docs/BUILD.md`). Ставится на машину, где не было ничего, — smoke-драйвер
  собирается и подписывается: `driver/emuusbaudio/build/smoke.sys`.
* `driver/kbuild.sh`: `-kernel -GS-`; порядок include'ов `km\crt → ucrt → um →
  shared → MSVC → km → shared` (иначе взрывается `_CRTIMP_ALT`); libs:
  ntoskrnl/hal/wdm/libcntpr + portcls/stdunk/ksguid/ks +
  wdfdriverentry/wdfldr (KMDF 1.35). Пути находятся сами (MSI-layout
  `Windows Kits\10`, либо NuGet `<root>/c/Include`); переопределение —
  `OED_VS/OED_MSVC/OED_KITS/OED_SDKVER/OED_WDFVER/OED_NTDDI`.
  Для исходников сэмпла нужен `-D_NEW_DELETE_OPERATORS_` (иначе placement-new
  из `NewDelete.cpp` не компилируется и линковка не сходится).
* Форматные таблицы: `speakerwavtable.h`/`micarraywavtable.h` + `minipairs.h` →
  24-бит в 4-байтовом контейнере (`KSDATAFORMAT_WAVEFORMATEXTENSIBLE`,
  `wValidBitsPerSample=24`), частоты 44.1/48/88.2/96/176.4/192k; драйвер
  выбирает USB-альт по частоте по таблице v1 (вывод emu-dump).
* Контролы: XU unit 0x0C..0x0F + Feature Unit 0x04, EP0 `SET_CUR` (0x21);
  `GET_CUR` (0xA1) firmware не реализует — сталл на любых class/vendor IN.
* INF `openemu-ks.inf`: bind whole-device (как openemu.inf), сервис
  EmuUSBAudio, `Include=ks.inf,wdmaudio.inf`,
  `Needs=KS.Registration,WDMAUDIO.Registration` + AddReg wdmaud (образец —
  emaud.inf из пакета 1.30.07).
* Тест-подпись: `bcdedit /set testsigning on` + REBOOT (спросить пользователя!),
  затем `pnputil`/devcon install, DbgPrint-проверки.
* В kernel-режиме устройство больше НЕ открывать WinUSB-папелью; `oed-mode.ps1`
  дополнить третьим режимом `kernel`.

## Не забыть

* `regsvr32 openEMUdriver.asio` остаётся.
* `driver/emuusbaudio/build/` — мусор от компиляции.
* `ref/Windows-driver-samples` — референс (minwavert* — построчный референс для
  правок); в репозитории его нет по лицензиям/размеру, поднимается
  `tools/get-ref.sh samples`, в git не входит (`ref/` в `.gitignore`).
* Подводные камни переключателя режимов: `pnputil /enum-drivers` локализован,
  парсить имя INF как целый токен (`openemu.inf` — подстрока
  `openemu-compat.inf`!); после переустановки пакета AddReg может не примениться
  к уже установленному devnode — прописывать `DeviceInterfaceGUIDs` вручную или
  удалять devnode с физическим replug.
