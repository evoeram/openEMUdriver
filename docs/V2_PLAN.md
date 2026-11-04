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

Описано конкретными числами 2026-11-04 в `driver/emuusbaudio/Emu/emu_ioctl.h`
(двойной заголовок: его же включает панель) и проверяется без железа
(`driver/utest.sh ioctl`):

* interface `{6DC68D18-D4C8-4DAC-A456-6CA921953BE8}`;
* `EMU_IOCTL_GET_CAPS` = `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED,
  FILE_READ_ACCESS)`, `EMU_IOCTL_GET_STATE` = тот же `0x801`;
* `EMU_CAPS_INFO` — 40 ULONG (160 Б), `EMU_STATE_INFO` — 13 ULONG (52 Б); только
  ULONG, ни указателя, ни 64-битного скаляра, поэтому раскладка не зависит от
  разрядности клиента; `Size`/`Version` первыми полями, `EMU_IOCTL_VERSION 1`;
* два набора бит: `SupportedMask` (что могло бы обеспечить дескриптор — единица
  XU на соответствующий SET) и `ImplementedMask` (что отвечает эта сборка).
  Панель предлагает только пересечение;
* `ValidMask` ответа state выводится из `EMU_NO_VALUE`-полей кэша, а
  `StateSequence` — счётчик принятых заметок; `PnpState` — числа
  `EMU_WIRE_STATE_*`, которым в драйвере сопоставлен `EMU_STATE`;
* произвольных `unitId`/`controlSelector`/длин в контракте нет: unit id
  возвращается драйвером в `EMU_CAPS_INFO` как факт описания, а не принимается
  от клиента;
* кодов записи в этом заголовке намеренно нет. Пока трейс с железа не показал,
  что CA0189 принимает `SET_CUR`, новый код был бы обещанием, которое драйвер не
  выполняет, а панель — кнопкой, бьющей в stall.

`EmuIoctlClassify` — решение без IRP: чужой control code уходит обратно PortCls
(запросы KS/wdmaud идут через тот же FDO другим типом устройства — `FILE_DEVICE_KS`
0x2f против наших 0x22), «не готово» опережает проверки буфера, пустой input и
output достаточного размера — иначе `STATUS_INVALID_PARAMETER` /
`STATUS_BUFFER_TOO_SMALL`. Обработчик `IRP_MJ_DEVICE_CONTROL`,
`IoRegisterDeviceInterface` и строки `.Interfaces` в INF — написаны
(см. «этап 4b»; на живом устройстве контракт по-прежнему не проверялся).

## Что реально есть сейчас

| Часть | Состояние |
| --- | --- |
| v1 ASIO/WinUSB | Зрелая часть: ASIO COM DLL, панель, диагностика, INF, packed-24 streaming, явное кадровое планирование. На 0404 (fw 8.04) заявлены 44.1/48/96/192 кГц, 2/4 канала |
| v2 kernel | Каркас: SimpleAudioSample, smoke.c, ручной kbuild.sh, план. Последний коммит — WIP |
| USB в kernel | **Стартует на живой 0404 и отвечает на IOCTL**: contract-602 клиент, GET_DESCRIPTOR (1832 Б) + парсер (выправлен по реальным байтам), транспорт class-запросов EP0, read-only пробник clock-rate (0xC000000D — разбирается), кэш четырёх контролов, контрольное устройство `\\.\EmuUSBAudioCtl` с roundtrip `GET_CAPS`/`GET_STATE` (`emu-probe: PASS`). Отсутствует: `SET_CUR` на живом железе, выбор конфигурации и alt, URB-кольцо, interrupt EP, feedback, iso-движок |
| PortCLS | Практически исходный сэмпл: stereo/16-bit/48 kHz, ToneGenerator, программный таймер, эмуляция DMA-позиции |
| INF / ASIO / панель v2 | Панель и ASIO не реализованы. Контракт панели описан, покрыт тестом и **проверен на железе через контрольное устройство** (`emu-probe: PASS`, `GET_CAPS`/`GET_STATE`); пробник — `driver/tools/emu_probe.c` (`driver/kprobe.sh`) |
| Воспроизводимость | M0 выполнен: `build.sh` и `driver/kbuild.sh` сами находят компилятор, WDK и SDK (локальные пути убраны), подпись вынесена в `driver/ksign.ps1`, `ref/` поднимается по запросу `tools/get-ref.sh`, дамп дескрипторов сохраняется `emu-dump -o`. Юнит-тесты двойных исходников — `driver/utest.sh` (парсер, EP0-запросы, кэш, IOCTL-контракт). Инструкция и фактические проверки — `docs/BUILD.md`. CI по-прежнему нет |

## Этапы и критерии готовности

Не по строкам кода, а по проверяемым этапам.

| Этап | Содержание | Критерий готовности |
| --- | --- | --- |
| M0. База и воспроизводимость | ✅ сделано 2026-11-04 (см. «Итог M0»): v1 tag'ом; убрать локальные пути из сборки (поиск тулчейна вместо MSBuild-проекта — `.vcxproj` из сэмпла ещё не подключены); отдельный signing-скрипт; восстановить или удалить ссылки на `ref/`; сохранять descriptor dumps | Чистая машина собирает v1 и smoke/v2 по инструкции — **выполнено на новой машине без WDK** |
| M1. PnP-каркас 0404 | 🟡 каркас написан и упакован (см. «Итог M1»); **критерий выполнен 2026-11-05**: 50 disable/enable + 10 remove/replug под Driver Verifier (Special Pool, IRQL, DDI compliance) — ни одного BSOD/дампа, ни медленных циклов, ни ошибок в System-логе (см. «Ksoak M2»); замечание по pool-дельте +1.1..1.7 МБ не монотонно — фоновый шум живой машины | 50 циклов disable/enable и remove/replug под Driver Verifier без утечек и BSOD — **выполнено** |
| M2. USB + управление | ✅ **выполнено 2026-11-05** (кроме SelectConfiguration/alt — отнесён к M3, стриминг): parser capabilities + кэш состояния + версионные IOCTL + control-DO + полный SET-путь (rate/clock/spdif/softlimit) — всё на живой 0404. GET_CUR не поддержан firmware (stall, как в v1) — кэш SET-only по дизайну. Выбор конфигурации/alt — M3 | Панель отдельно меняет rate/clock/S/PDIF/soft limit — **выполнено** (`emu-probe set-*` на живом устройстве, descriptor/domain vetoes отказывают вне диапазона); WinUSB одновременно не открывается (kernel-драйвер владеет devnode) |
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
  `__imp_TerminateProcess` и т. п.). ЗАКРЫТО 2026-11-04 на M1: дело было не в
  исходниках, а в линковочной линии — `-NODEFAULTLIB:LIBCMT.lib` +
  `BufferOverflowK.lib` дают kernel-реализацию `__report_gsfailure`, и дерево
  линкуется целиком (`sample.sys`, 91648 Б, импорты `ntoskrnl.exe`/`HAL.dll`/
  `portcls.sys`). В цель каркаса `hw.cpp`/`savedata.cpp`/`ToneGenerator.cpp`
  всё равно не входят — M1 по плану PnP-скелет без обвязки; теперь это выбор, а
  не блокер. `.vcxproj`/MSBuild из сэмпла не подключены: пока `kbuild.sh`
  покрывает требование воспроизводимости, MSBuild-проект — отдельная задача.
* CI и автотестов нет (M0 их и не обещал).

Эмпирика по установкам: NuGet `Microsoft.Windows.SDK.CPP.x64` содержит только
библиотеки (`c/ucrt/x64`, `c/um/x64`) — заголовки `ucrt/um/shared` даёт SDK MSI;
NuGet `Microsoft.Windows.WDK.x64` (105 МБ) — это `km`-заголовки, `km`-либы и
`wdf/kmdf` до 1.35. Символы `WdfFunctions_01035`/`WdfDriverGlobals` даёт
`wdfdriverentry.lib`.

## Итог M1 (2026-11-04)

Каркас написан, собирается, упаковывается в валидный подписанный пакет.
**Критерий этапа (50 циклов под Driver Verifier) не выполнен** — на этой машине
нет 0404 и нет админ-права, а `bcdedit /set testsigning on` требует
перезагрузки. Это единственный незакрытый пункт M1; порядок прогона —
`docs/BUILD.md` §9.

Сделано:

* `driver/emuusbaudio/Emu/{emu.h,emu_driver.cpp,emu_pnp.cpp}` — отдельное
  дерево вместо правки `Main/adapter.cpp`; `driver/kbuild.sh` добавил
  `$DRV/Emu` в `INCLUDE`. Результат: `emuusbaudio.sys` 7168 Б,
  `/imports` → только `ntoskrnl.exe` (14 функций) и `portcls.sys` (4):
  `PcInitializeAdapterDriver`, `PcAddAdapterDevice`,
  `PcGetPhysicalDeviceObject`, `PcDispatchIrp`. KMDF не импортируется вообще,
  поэтому в INF нет `[Install.NT.Wdf]`/`KmdfService`, а открытая проблема M0
  (user-mode `/GS` из `LIBCMT`) обходится: `hw.cpp`/`savedata.cpp`/
  `ToneGenerator.cpp` в цель не входят.
* Раскладка PortCls-расширения проверена на месте, а не «на веру»: our context
  лежит за `PORT_CLASS_DEVICE_EXTENSION_SIZE` (512 Б на x64), `AddDevice`
  просит `PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(EMU_DEVICE_CONTEXT)`;
  `StartDevice` принимает расширение только если `Signature` равен 0 (свежий,
  `IoCreateDevice` обнуляет) либо нашему значению **и** `PcGetPhysicalDeviceObject`
  совпадает с PDO-слотом PortCls. Иначе — `STATUS_DEVICE_CONFIGURATION_ERROR`:
  неверное предположение о раскладке валит старт, а не ломает память под
  Verifier'ом.
* Жизненный цикл: `StartDevice` идемпотентен (disable/enable переиспользует тот
  же FDO, remove lock инициализируется один раз, счётчики старт/стоп/remove
  переживают цикл); на `IRP_MN_REMOVE_DEVICE` — `IoReleaseRemoveLockAndWait`
  **до** `PcDispatchIrp`, затем сразу `return` (FDO и расширение умирают
  внутри PortCls, после форвардинга трогать нельзя); `SURPRISE_REMOVAL` ничего
  не освобождает — за ним всегда приходит REMOVE; `QUERY_STOP`/`CANCEL_STOP`/
  `QUERY_REMOVE`/`CANCEL_REMOVE` соглашаются и отдаются PortCls.
* Трассировка: `_DbgPrintF` в free-сборке — пустой макрос, а `DEBUGLVL_*`/
  `TRACE_LEVEL_*` из `ksdebug.h` под `_NTDDK_` не объявлены → свой `EMU_TRACE`
  через `DbgPrintEx(DPFLTR_IHVDRIVER_ID, …)` с уровнем из
  `<RegistryPath>\Parameters\TraceLevel` (читается в `DriverEntry`, кламп
  ERROR..BLAB). `DriverUnload` chain'ится к PortCls'овскому.
* `inf/openemu-ks.inf` — только `USB\VID_041E&PID_3F04`, whole-device (CA0189
  прячет UAC за vendor-class 0xFF, дробить на MI_* нечего), `Class=Media`,
  `Include=ks.inf`/`Needs=KS.Registration`, `PnpLockdown=1`;
  `WDMAUDIO.Registration` намеренно нет до M4. `AssociatedFilters`
  (`wdmaud,swmidi,redbook`) из M1-INF тоже убрано: эти фильтры вешаются на FDO и
  ищут KS-фильтры, которых каркас не регистрирует, — в соаке они подмешали бы
  чужие отказы к нашим. Вернутся в M4 вместе с WaveRT/topology.
  `Inf2Cat /os:10_X64` — 0 ошибок, 0 предупреждений.
* `driver/kinstall.ps1` — стейджинг в `driver/package-ks/` (в `.gitignore`) →
  подпись `.sys` → `Inf2Cat` → подпись `.cat` → снятие `openemu.inf`/
  `openemu-compat.inf` (претендуют на тот же devnode) → `pnputil /add-driver
  /install` → статус devnode/сервиса/событий; `-Uninstall` возвращает v1 WinUSB.
  Прогнано на этой машине: всё до `pnputil` проходит без повышения прав, `.sys`
  и `.cat` подписаны тестовым сертификатом `E01D3BA3B04D…`.
* `driver/ksoak.ps1` — приёмочный харнесс: `-Cycles 50` disable/enable с
  ожиданием `CM_PROB_DISABLED` (22) и `OK`, `-RemoveCycles 10`
  `/remove-device` + `/scan-devices`, nonpaged pool до/после через
  `Win32_PerfFormattedData_PerfOS_Memory` (WMI-имена не локализованы, в отличие
  от счётчиков `Get-Counter`), детект медленных циклов (>3 с — признак
  зависшего remove lock), поиск ошибок `emuusbaudio`/`portcls` в System.
  `-Verifier`/`-VerifierOff` только печатают команды: ни `verifier`, ни
  `bcdedit`, ни перезагрузку скрипт не трогает.

Подводные камни, найденные на этапе (не менять без причины):

* `IoDeleteRemoveLock` в заголовках 10.0.26100 **нет**; в free-сборке у
  `IO_REMOVE_LOCK` нет отладочного блока, поэтому явного удаления и не требуется.
* `extern "C" PCPFNSTARTDEVICE EmuStartDevice;` объявляет **переменную**
  типового указателя на функцию, а не функцию — только явный прототип.
* `Inf2Cat` из 10.0.26100: `/verbose` → «Parameter format not correct», токенов
  `11_*` нет (Windows 11 покрывается `10_X64`).
* `pnputil /enum-drivers` локализован → разбор по имени INF как целому токену
  (наследие `inf/oed-mode.ps1`, воспроизведено в `kinstall.ps1`).
* `DriverEntry` обязан быть в `#pragma code_seg("INIT")` (образец так и делает);
  после этого `dumpbin /headers` даёт `CA000040` → Initialized Data,
  **Discardable**, а `PAGE` — `60000020` Code | Execute Read без бита
  `Not Paged`, т. е. действительно страниц. Проверять после каждой смены
  `code_seg`, а не на глаз.

Что проверим первым на живой машине: что layout-гвард не срабатывает (иначе
devnode в problem'е и `STATUS_DEVICE_CONFIGURATION_ERROR` в логе) и что PortCls
довольен `MaxObjects=2` при нуле вызовов `PcRegisterSubdevice` — M1 не
регистрирует KS-объектов совсем. Физические replug'ы обязательны отдельным
прогоном: `/remove-device` + `/scan-devices` не воспроизводят surprise-remove
посреди IO. Пока каркас занят ядром, v1 WinUSB к устройству не открывается —
это ожидаемо и есть причина, по которой `kinstall.ps1` снимает v1-пакет, а
`-Uninstall` его возвращает.

## Подготовка M2: USB-путь проверен (2026-11-04)

M2 упирается в один вопрос — умеет ли наш харнес вообще говорить по USB из
ядра. Проверено без железа (пробный TU, в репозиторий не кладётся: он ничего
не устанавливает, `DriverEntry` возвращает `STATUS_DEVICE_NOT_READY`):

* Две разные библиотеки, обе добавлены в `kbuild.sh`:
  * `usbd.lib` — import-библиотека **USBD.SYS** (модуль реально лежит в
    `System32\drivers`, 85 КБ) для легаси-хелперов:
    `USBD_CreateConfigurationRequestEx`,
    `USBD_ParseConfigurationDescriptorEx`,
    `USBD_ValidateConfigurationDescriptor`, `USBD_QueryBusTime`. В образе
    появляется импорт `USBD.SYS`.
  * `usbdex.lib` — **статическая** реализация нового handle-API:
    `USBD_CreateHandle(USBD_CLIENT_CONTRACT_VERSION_602)`,
    `USBD_UrbAllocate`/`USBD_UrbFree`, `USBD_IsochUrbAllocate`,
    `USBD_SelectConfigUrbAllocateAndBuild`,
    `USBD_SelectInterfaceUrbAllocateAndBuild`,
    `USBD_AssignUrbToIoStackLocation`, `USBD_CloseHandle`. Она сама находит
    интерфейс `GUID_USBD_INTERFACE` через `IRP_MN_QUERY_INTERFACE`, поэтому
    новых импортируемых модулей в образе нет — только `ntoskrnl.exe`
    (`IoBuildDeviceIoControlRequest`, `IoAllocateIrp`, `KeWaitForSingleObject`,
    `RtlQueryRegistryValues`, `MmIsDriverVerifying`, `KeBugCheckEx`).
* Цена `usbdex.lib`: он собран с `/GS`, поэтому для линка обязательно
  `-NODEFAULTLIB:LIBCMT.lib` + `BufferOverflowK.lib`; без них — та же стена M0
  (user-mode `gs_report` с `__imp_TerminateProcess`). Тем самым закрыт и вопрос
  «линкуется ли сэмпловое дерево» — линкуется (`sample.sys`, 91648 Б, импорты
  `ntoskrnl.exe`/`HAL.dll`/`portcls.sys`).
* Типы URB под M2/M3 подтверждены компиляцией:
  `struct _URB_CONTROL_TRANSFER` (`PipeHandle = NULL` для EP0,
  `SetupPacket[8]`, `TransferFlags = USBD_TRANSFER_DIRECTION_OUT`),
  `struct _URB_ISOCH_TRANSFER` (`StartFrame`, `NumberOfPackets`, `ErrorCount`,
  `IsoPacket[].Offset/Length/Status`; `USBD_START_PIPELINE_FILTER` в этом SDK
  **нет** — для render просто `USBD_TRANSFER_DIRECTION_OUT`),
  `struct _URB_SELECT_CONFIGURATION` (`ConfigurationDescriptor`,
  `ConfigurationHandle`, встроенный `Interface`),
  `USBD_INTERFACE_LIST_ENTRY`/`USBD_INTERFACE_INFORMATION`.
* Что проверкой **не** является: выбор стековой локации для
  `USBD_AssignUrbToIoStackLocation` (в пробе — `IoGetNextIrpStackLocation` для
  IRP из `IoBuildDeviceIoControlRequest`) и реальная семантика
  `USBD_INTERFACE_LIST_ENTRY` (терминатор `{NULL,NULL}`, `Interface.Length` под
  число endpoint'ов). Закрыто 2026-11-04 по документации, а не по догадке:
  «How to Submit an URB» требует `IoAllocateIrp(Target->StackSize, …)` +
  `IoGetNextIrpStackLocation` в `IRP_MJ_INTERNAL_DEVICE_CONTROL` с
  `IOCTL_INTERNAL_USB_SUBMIT_URB`, затем `USBD_AssignUrbToIoStackLocation` и
  `IoSetCompletionRoutineEx` на том же target; `USBD_CreateHandle(FDO,
  StackDeviceObject=PDO, 602, tag, &h)`. Семантика
  `USBD_INTERFACE_LIST_ENTRY` для `SelectConfiguration` — остаётся первым
  вопросом на живом устройстве.
* `USBD_QueryBusTime` помечен `__drv_preferredFunction("Obsolete")`; для
  `StartFrame` в M3 предпочтителен URB
  `URB_FUNCTION_GET_CURRENT_FRAME_NUMBER` (`struct
  _URB_GET_CURRENT_FRAME_NUMBER`) — сравнить оба на железе, а не верить
  комментарии из заголовка.

Форма M2 с этой базой: `StartDevice` → `USBD_CreateHandle(FDO, PDO)` →
`GET_DESCRIPTOR(configuration)` через URB → чистый парсер дескрипторов
(юнит-тест в user-mode по фикстуре — плановый тест №1) →
`SelectConfiguration`/`SelectInterface` на vendor-class интерфейсы → кэш
состояния + EP0 `SET_CUR` (протокол в `docs/PROTOCOL.md`) → версионные IOCTL и
device interface.

### Парсер дескрипторов и плановый тест №1 (2026-11-04)

Из всей цепочки M2 реализован и проверен её центр — разбор configuration
descriptor. `driver/emuusbaudio/Emu/emu_desc.h` + `.cpp`: вход — raw block из
`GET_DESCRIPTOR`, выход — `EMU_DEVICE_CAPS` (роли интерфейсов, AC-статусный EP,
терминалы, FEATURE_UNIT, EXTENSION_UNIT с `wExtensionCode`, по каждому alt:
направление, каналы/подфрейм/битность, список частот либо continuous-диапазон,
data-EP с `wMaxPacketSize`/attributes/usage, feedback-EP). Байты читаются
смещениями без структурных кастов, поэтому один и тот же TU собирается и в
образ (`-DEMU_KERNEL` → `<ntddk.h>`), и в user-mode (`<windows.h>` +
`<ntstatus.h>`).

Сознательные правила, которые тест проверяет поштучно:

* `wTotalLength` обязан равняться длине переданного блока; короткий хвост,
  `bLength < 2` и выход за блок — `STATUS_DATA_ERROR`, а не «разобрали что
  успели».
* alt 0 (zero-bandwidth) не записывается никогда; alt без `AS_GENERAL`, без
  PCM-формата или без data-EP — `STATUS_DEVICE_CONFIGURATION_ERROR`, потому что
  M3 иначе построил бы несуществующий пайплайн.
* направление считается по `bTerminalLink` (`OUTPUT_TERMINAL` → render,
  `INPUT_TERMINAL` → capture) и сверяется с битом направления data-EP;
  расхождение — ошибка.
* переполнение любого лимита (`*_MAX_*`) — `STATUS_BUFFER_OVERFLOW`, тихого
  отбрасывания записей нет; ровно на границе парсер обязан проходить.
* роли выводятся из `bInterfaceSubclass` (CA0189 прячет только класс,
  `0xFF` + subclass 1/2 — тот же признак, на котором стоит v1); оба layout
  `EXTENSION_UNIT` распознаются, фактический записан в `Caps`.
* при неудаче `Caps` пересобирается в исходное пустое состояние: один
  `EMU_DEVICE_CAPS` живёт между циклами start/stop и «почти правильный» вывод
  хуже отсутствия вывода.

Запуск: `cd driver && ./utest.sh desc` — сначала компиляция парсера флагами
`kbuild.sh` (`-kernel -W3 -WX`, строка `KERNEL-COMPILE: … ok`), затем
user-mode-тест `driver/tests/emu_desctest.cpp`. Фактический результат:
`142 checks, 0 failures / RESULT: PASS`, обе kernel-цели после изменений
собираются (`smoke.sys`, `emuusbaudio.sys`).

Покрыто: malformed lengths, лишние/переставленные endpoint'ы, превышение
размеров массивов, оба XU-layout, continuous-частоты, non-PCM alt. Не покрыто
и требует железа: «реальные дескрипторы всех моделей» — фикстура 0404 собрана
по замерам `docs/RE_NOTES.md`; как только `emu-dump -o docs/descriptors` даст
raw hex, она заменяется дампом.

### USB-клиент M2, этап 1: дескриптор с живого устройства (2026-11-04)

`driver/emuusbaudio/Emu/emu_usb.cpp` — contract 602 поверх `usbdex.lib`:
`USBD_CreateHandle(Fdo, Pdo, 602, EMU_POOLTAG)` в `StartDevice`, синхронная
отправка URB (`IoAllocateIrp(Pdo->StackSize)` → `IoGetNextIrpStackLocation` →
`IRP_MJ_INTERNAL_DEVICE_CONTROL` + `IOCTL_INTERNAL_USB_SUBMIT_URB` →
`USBD_AssignUrbToIoStackLocation` → `IoSetCompletionRoutineEx` →
`IoCallDriver(Pdo)` → `KeWaitForSingleObject`), два `GET_DESCRIPTOR`
(configuration): сначала 9 байт, чтобы узнать `wTotalLength`, затем ровно
столько же; расхождение `wTotalLength` между чтениями и короткий transfer —
ошибка, не «частичный успех». `EMU_DEVICE_CAPS` (~2.5 КБ) — `ExAllocatePool2`
в nonpaged, в расширении FDO только указатели. `StopDevice` и `RemoveDevice`
вызывают `EmuUsbRelease` (идемпотентен: каждый ресурс обнуляется при
освобождении), поэтому 50 циклов start/stop из M1-соака не текут.

Что этот этап даёт помимо самой цепочки: разобранную таблицу (`config/iface/alt/XU`
одной строкой на запись) драйвер печатает при `TraceLevel >= 4`, raw hex
дескриптора по 16 байт на строку — при `>= 5`. Это и есть способ получить
настоящие дескрипторы 0404 в `docs/descriptors` без WinUSB-панели, которую
нельзя открывать, когда к устройству привязан ядерный драйвер.

`SelectConfiguration`/`SelectInterface` сюда намеренно не входят: они меняют
состояние шины, и их параметры (`USBD_INTERFACE_LIST_ENTRY`, `Interface.Length`
под число endpoint'ов) надо сверять на трейсе живого устройства, а не писать
дважды на глазок.

Проверено: `emuusbaudio.sys` собирается и линкуется с пятью TU
(`emu_driver.cpp emu_pnp.cpp emu_usb.cpp emu_desc.cpp emu_ctrl.cpp`),
`dumpbin /imports` —
только `ntoskrnl.exe` и `portcls.sys` (usbdex статический, `USBD.SYS` в
импортах не появляется); `smoke.sys`, `sample.sys` и `./utest.sh` (2 из 2
бинарника зелёные) после изменений тоже проходят.
Проверено **не** является загрузкой на железе: M1-акцептанс (testsigning,
Verifier, soak) до сих пор не выполнен, и именно он покажет, что тракт
`USBD_CreateHandle → GET_DESCRIPTOR` проходит на 0404.

### Контрольные запросы EP0: builders и тест (2026-11-04)

`driver/emuusbaudio/Emu/emu_ctrl.h` + `.cpp` — второй исходник двойного
назначения: сборка
class-запросов CA0189 (`bmRequestType` 0x21/0xA1, `bRequest` CUR,
`wValue = (CS<<8)|channel`, `wIndex = (unitID<<8)|AcInterface`, `wLength`) и
кодеков значений: index частоты 0..5, 4-байтный big-endian rate цифрового входа,
SInt16-громкость в 1/256 дБ. Отправки здесь нет — модуль не ссылается ни на USB,
ни на ядро, и потому собирается и в образ, и в user-mode-тест одним и тем же
исходником; транспорт — `EmuUsbSendControl` в `Emu/emu_usb.cpp` (см. этап 2).

Одно правило, которое проверяется отдельно: отказ сборки обнуляет весь запрос.
`bmRequestType == 0` не называет ни одного class-контроля, так что вызов,
проигнорировавший статус, упрётся в stall нулевого EP0 вместо молчаливой смены
частоты или лимитера. Для `EmuCtrlBuildClockRateSet(44000)` это принципиально:
без обнуления остался бы готовый SET_CUR с index 0, то есть 44100.

`driver/tests/emu_ctrltest.cpp` — 123 проверки, `RESULT: PASS`. Ассерт идёт по
восьми байтам setup-пакета, а не по кодам возврата: поле, сдвинутое на байт,
адресует другой контроль, и устройство отвечает на корректный неверный запрос
сменой состояния. Ожидаемые байты — раскладка v1-движка (`src/emu_usb.cpp:311-360`),
который на этом железе работает, а не спецификация «в среднем»: там же проверены
границы (`unitID`/`channel` 0x100 отклоняются, 0xFF — нет; `wLength` 20 проходит,
21 — нет), кодировка −127 дБ (`0x8100` → `00 81`) и знакового минимума,
`GET_CUR` lock/rate/format и `SET_CUR` rate/source/soft limit/format.

Тест заодно поймал две собственные ошибки ожидания (192000 в BE — это
`00 02 EE 00`, а `80 80` — не −127 дБ) и одну настоящую в коде: путь
«нет index для этой частоты» возвращал статус, оставляя собранный запрос
отправляемым. Исправлено до зелёного прогона.

Чего этап не доказывает: что железо принимает эти байты (для 0404 — только по
трейсу M1-акцептана) и что `GET_CUR` вообще отвечает: v1 фиксирует stall на
`0xA1` (docs/PROTOCOL.md), поэтому читалка состояния — тоже проверка на железе,
не на фикстуре. Для этого она вынесена в отдельный необязательный пробник
(см. этап 2), а не вшита в тракт открытия устройства.

### USB-клиент M2, этап 2: транспорт class-запросов и пробник (2026-11-04)

`EmuUsbSendControl` (`Emu/emu_usb.cpp`) — `struct _URB_CONTROL_TRANSFER` поверх
уже доказанного `EmuUsbSubmitUrb`: `Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER`,
`PipeHandle = NULL` (нулевой адресат описывается самим setup-пакетом — интерфейс
и юнит лежат в `wIndex`), setup из `EMU_CTRL_REQUEST`, направление по
`bmRequestType` (`USBD_TRANSFER_DIRECTION_OUT` / `IN | SHORT_TRANSFER_OK`),
`TransferBuffer` = буфер вызывающей стороны. Короткий data-stage разрешён на
уровне USB, но здесь же сверяется длина: несовпала — `STATUS_DEVICE_CONFIGURATION_ERROR`,
stall — `STATUS_DEVICE_HARDWARE_ERROR` с usb-статусом в логе. Это ровно та
семантика, на которой парсер отказывается считать успехом частичный transfer.

`EmuUsbProbeClockRate` — первый собственный запрос драйвера, и он ничего не
меняет: `GET_CUR` clock-rate на том unit id, который назвал парсер. Неудача не
приводит к отказу `StartDevice` (v1 говорит, что это железо не отвечает на
`0xA1`), значение пишется одной TERSE-строкой вместе с index и Hz. Смысл именно
в этой строке: за тот же прогон M1-акцептанса становится известно, читается ли
состояние с нулевого EP0 из ядра. Это и есть развилка для кэша состояния: если
отвечает — зеркало наполняется чтениями, как в v1; если нет — только из
`SET_CUR`, и панель не может показать реальное значение, которое кто-то поменял
снаружи.

Проверено: image 30208 → 33280 Б, `dumpbin /imports` по-прежнему
`ntoskrnl.exe` + `portcls.sys`; сборка идёт без warning'ов, `./utest.sh` —
2 из 2, `smoke.sys` и `sample.sys` линкуются. Не проверено решительно ничего
на железе: `SET_CUR` отсюда не отправляется (он меняет состояние устройства и
ждёт того же трейса, что и `SelectConfiguration`), а data-stage длиннее одного
байта этим путём ещё не ходил.

### USB-клиент M2, этап 3: вопросы к таблице частот и кэш состояния (2026-11-04)

Этап 2 оставил развилку («читается ли состояние с EP0»), но не место, где ответ
хранится. Добавлены ещё два двойных исходника — оба и в образе, и под тестом.

`Emu/emu_desc.cpp` пополнился вопросами к уже разобранной таблице:
`EmuAltSupportsRate` (дискретный список должен назвать частоту ровно; непрерывный
диапазон отвечает «да» на всё между минимумом и максимумом включительно),
`EmuCapsFindAlt(Caps, Direction, Channels, RateHz)` и `EmuCapsSupportsRate`.
`EmuCapsFindAlt` перебирает alt'ы в порядке дескрипторов и отдаёт первый
подходящий: выбор детерминирован для конкретного устройства и не зависит от того,
в каком порядке заполнилась таблица. Формат (SubFrameSize/BitResolution) он не
учитывает — alt подбирает вызывающая сторона под тот, который она умеет
заполнять. `EmuCapsSupportsRate` сформулирована сознательно слабо
(«частота встречается где-нибудь»), потому что такт в CA0189 один на всё
устройство, а какие потоки откроются в момент чтения состояния неизвестно;
per-direction правда добывается только из `EmuCapsFindAlt`. Нулевая частота не
проходит никогда: парсер отвергает 0 и в дискретном списке, и в границе
диапазона, так что TRUE означает реальное значение из дескриптора. Именно это и
не даёт битому байту из GET'а притвориться 44100 Гц.

`Emu/emu_state.cpp` — четыре панельных контрола в одном `EMU_SETTINGS` (rate Hz,
clock source, digital format, soft limit), где `EMU_NO_VALUE` значит «значение
никогда не было установлено», а не «ноль». Маркер нужен ровно из-за железа:
`GET_CUR` на CA0189 стоит (docs/PROTOCOL.md), поэтому поле может знать только то,
что драйвер видел сам — удачное чтение или собственный `SET_CUR`. Обнулённая
структура выглядела бы как «0 Гц, внутренний такт, soft limit выключен», и панель
показала бы это как факт; `EmuSettingsInit` такой возможности не оставляет и
вызывается и на старте, и в `EmuUsbRelease` — после release устройства никаких
доказательств не остаётся.

`EmuSettingsNoteRate` — всё правило кэша целиком: частоту, против которой
говорит дескриптор, не записываем, и уже известное значение при отказе не
трогаем. Отсюда и безопасность пробника: индекс вне таблицы декодируется в 0 Гц
(`EmuCtrlRateFromIndex`), а отказ записать 0 сохраняет ранее известную частоту.
Три байтовых контрола будет записывать SET-путь — он по-прежнему ждёт трейса;
поля объявлены здесь, чтобы правило «unknown — это не 0» было записано один раз.
Синхронизации у кэша пока нет и она не нужна: единственный писатель сейчас —
`StartDevice` на PASSIVE_LEVEL, до того как у устройства появляется IOCTL-клиент.
С версионными IOCTL (тот же отложенный шаг) придётся выбирать: либо spin-замок на
четыре ULONG, либо атомарные обмены по полю — плановый тест №6 («панель меняет
настройки во время потока») решит это не раньше, чем появится сама панель.

Тесты (`driver/utest.sh`, теперь три бинарника): к парсеру добавился случай
alt-lookup — 182 проверки, среди них 4ch@192 кГц обязан вернуть NULL
(единственная комбинация, которую M5 требует корректно отклонять), границы
непрерывного диапазона включительно, и дискретный + непрерывный alt в одном
устройстве дают предсказуемый ответ. `driver/tests/emu_statetest.cpp` — 42
проверки против разобранного, а не заполненного руками `EMU_DEVICE_CAPS`;
фикстура 0404 вынесена в `driver/tests/descfixture.h`, чтобы у обоих тестов был
один и тот же device. `utest.sh` умеет линковать на единицу теста несколько
модулей: `state` собирает `emu_state.cpp` **и** `emu_desc.cpp` — иначе проверка
«кэш следует за дескриптором» была бы проверкой собственных ожиданий теста.

Проверено: image 33280 → 33792 Б, `dumpbin /imports` по-прежнему
`ntoskrnl.exe` + `portcls.sys`, сборка без warning'ов, `./utest.sh` — 3 из 3,
`smoke.sys` и `sample.sys` линкуются. На железе не проверено по-прежнему ничего
из того же списка, что и в этапе 2: чем отвечает `GET_CUR` clock-rate на живом
0404 (то есть наполняется ли кэш чтениями), и принимает ли устройство те alt'ы,
которые назвал парсер.

### USB-клиент M2, этап 4a: контракт панели описан числами и проверен (2026-11-04)

Раздел «IOCTL-контракт» требовал закрыть набор операций до того, как появится
панель, которая будет его звать. Закрыт кодом, а не текстом: `Emu/emu_ioctl.h` —
единственный заголовок, который знают и драйвер, и будущая панель;
`Emu/emu_ioctl.cpp` — правило, но не IRP. Третий двойной исходник в том же
смысле, что `emu_desc.cpp` и `emu_state.cpp`: он компилируется флагами ядра
обязательно, поэтому проверяемое правило и есть то, что уедет в образ.

Почему решение вынесено из обработчика. Копирование буфера и вызов
`PcDispatchIrp` без живого устройства не воспроизвести, а ошибка в этом месте — не
«панель показала ерунду», а неправильный ответ чужому стеку: тот же
`EmuPnpHandler` из M1 живёт ровно потому, что всё остальное он отдаёт PortCls
нетронутым. Поэтому `EmuIoctlClassify` получает три числа (control code, длины
входного и выходного буфера) и флаг «устройство пригодно», а возвращает
действие + статус + размер ответа. Чужой код — это `EmuIoctlForward`: ничего не
отгадывать и не отвергать по диапазону. Соседняя функция `0x802` в том же типе
устройства тоже уходит в PortCls — сборка, которая его не определила, не вправе
притворяться его владельцем. Проверено, что KS-запросы (`FILE_DEVICE_KS` = 0x2f,
`shared/ks.h:74`) не пересекаются с нашими 0x22: иначе пересылка была бы дырой,
а не защитой.

`EMU_CAPS_INFO` заполняется из разобранных `EMU_DEVICE_CAPS`, а не из
предположений теста: `RenderAltCount`/`CaptureAltCount` — из `EmuCapsAltCount`,
список частот — из `EmuCapsRateList` (дедуп по всем alt'ам, по возрастанию,
`EMU_RATES_CONTINUOUS`/`EMU_RATES_TRUNCATED` вместо правки на глаз), unit id —
из `EmuCapsXuUnitId`. `SupportedMask` говорит про железо, `ImplementedMask` —
про сборку, и тест настаивает, что на 0404 они не равны: все четыре SET-единицы
на месте, ни один SET не отвечает. Остаток массива `Rates` обнуляется намеренно:
METHOD_BUFFERED отдаёт клиенту весь размер структуры, и не записанные хвосты
были бы содержимым ядра.

В `EmuState` к этим же полям добавился `Sequence`: счётчик принятых заметок,
нулевой после `EmuSettingsInit`. Нужен он, чтобы опрос не терял изменение,
которое вернуло прежнее значение (48 → 96 → 48 кГц выглядит как «ничего не
произошло»), а в `EMU_STATE_INFO` он переезжает как `StateSequence` рядом с
`StartCount`.

`EmuCapsRateList` — та часть парсера, что раньше была не нужна: пробник
опрашивал одну частоту, панели нужен список. Заодно первый случай в этом плане,
где «нет ответа» отличается от «пустой ответ»: у непрерывного alt'а перечислимого
членства нет, поэтому `RateCount` = 0 плюс флаг, а не молчаливое «частот нет».

Тесты: `driver/utest.sh` — четыре бинарника, `ioctl` — 163 проверки (пересылка
чужих кодов, порядок отказов, размеры буферов, заполнение caps на 0404 и на
устройстве без XU-единиц, unknown/known в state, смещения полей). У парсера
добавился случай rate-list (209 проверок: дедуп, усечение с флагом,
`MaxRates` = 0, непрерывный диапазон, дискретный + непрерывный вместе), у кэша —
счётчик заметок (58). Отдельно зафиксировано смещения: панель — отдельный бинарь
и компилируется в другой раз, поэтому переставленное поле должно ронять сборку,
а не показывать на экране чужое число. Заодно шаг 1 для `emu_ioctl.cpp`
подтверждает, что `DEFINE_GUID` из этого заголовка компилируется флагами ядра —
без этого этап 4b начал бы с вопроса, а не с ответа.

В образ `emu_ioctl.cpp` ещё не входит — подключается вместе с обработчиком
(этап 4b), мёртвый объект в линке ничего бы не доказал. Проверено: сборка
без warning'ов, `smoke.sys` и `sample.sys` линкуются, `./utest.sh` — 4 из 4.
На железе не проверено по-прежнему ничего из списка этапа 2 и 3.

### USB-клиент M2, этап 4b: обработчик IOCTL и control interface (2026-11-04)

Контракт соединён с IRP-миром. `Emu/emu_disp.cpp` — обработчик
`IRP_MJ_DEVICE_CONTROL`: чужие коды (KS/wdmaud и всё неизвестное) —
`PcDispatchIrp` нетронутыми, ровно как они ходили до обработчика; свои —
`EmuIoctlClassify` + заполнение `SystemBuffer` через `EmuIoctlFillCaps`/
`EmuIoctlFillState`, `Information = OutputBytes`. Правило «когда экземпляр
вообще отвечает» вынесено из обработчика в проверяемый модуль:
`EmuIoctlIsDeviceUsable(State, Caps)` — только `EMU_WIRE_STATE_STARTED` с
неравным NULL `Caps`; обработчик передаёт ему `State` атомарным чтением и
`Caps` напрямую, а тест (`utest.sh ioctl`, теперь 171 проверка) прогоняет все
пять состояний плюс оба «начат, но USB-клиент уже освободился» варианта.

Три решения этапа, которые иначе было бы не поймать:

* **StateLock (`FAST_MUTEX` в контексте) вместо надежды на remove lock.**
  Remove lock защищает от REMOVE, но не от STOP: панель, открывшая handle до
  disable/enable, может оказаться внутри `GET_CAPS` в тот же момент, когда
  `EmuUsbRelease` освобождает `Caps` — при Special Pool это мгновенный BSOD,
  без него — чтение освобождённой памяти. Но причина глубже: разорванный на
  два поля ответ — это ровно та ложь «0 Гц при валидной маске», против
  которой всё существование `EMU_NO_VALUE`. Под замком теперь весь жизненный
  цикл `Caps` и `Settings`: публикация в start (весь alloc+parse под замком,
  читатель видит NULL или целую таблицу), все освобождения в release,
  `EmuSettingsNoteRate` пробника, оба fill-пути. `USBD_CloseHandle` — вне
  замка (ему нужен PASSIVE_LEVEL, замок работает на APC). Это и есть выбор из
  этапа 3 («spin-замок или атомарные обмены»), назначенный по факту появления
  IOCTL-клиентов.
* **Обработчик — в непейджируемом `.text`, не в `PAGE`.** Он стоит перед
  `PcDispatchIrp` для каждого device-control IRP стека, а KS property-трафик
  PASSIVE_LEVEL не гарантирует; PAGED_CODE() в этой точке — скрытый bugcheck
  в checked-сборке. Свой fill-путь проверяет IRQL сам (после пересылки чужих
  кодов): DISPATCH-вызов нашего кода получает отказ, а не bugcheck под
  FAST_MUTEX. Секции после сборки проверены: `.text` с битом Not Paged,
  `PAGE` = `60000020`, `INIT` — discardable.
* **REMOVE при так и не стартовавшем экземпляре больше не вызывает
  `EmuUsbRelease`.** Начатый и упавший на layout-гварде экземпляр имеет
  Signature == 0 и неинициализированный StateLock — старый вызов был
  бесплатным no-op'ом по нулевым полям, теперь это неинициализированный мьютекс.
  Guard по Signature: ресурсов нет — освобождать нечего.

Control interface. `IoRegisterDeviceInterface(PDO)` — один раз на экземпляр,
но попытка повторяется, пока имени нет (старт, упавший до регистрации,
повторяется сам). Включение — только после `State = Started` (панель, выигравшая
гонку с enable, читает рабочее устройство, а не «полустарт»); выключение — на
STOP (до release: открывающий во время стопа получает «no such device», а не
устройство, отвечающее «not ready») и перед REMOVE, имя освобождается до
`PcDispatchIrp` — после форвардинга расширение недостижимо. На
SURPRISE_REMOVAL интерфейс **намеренно** остаётся включённым: открытая панель
до завершения remove получает наши честные отказы «device not ready», и
регистрацию закрывает сам PnP-менеджер. Ошибка регистрации — провал старта
(этап 8 control-plane-цепочки, откат `EmuUsbRelease` в обратном порядке);
ошибка включения — только WARN: регистрация стоит, устройство живо, сломан
только доступ панели.

Прочее по этапу: `C_ASSERT` связал `EMU_STATE` с `EMU_WIRE_STATE_*`
(emu.h — обещание из emu_ioctl.h теперь проверяется компиляцией); GUID
определяется ровно в одном TU (`<initguid.h>` — с этапа 4c в
`emu_driver.cpp`, который передаёт его `IoCreateDeviceSecure`);
список исходников
в `kinstall.ps1 -Build` и BUILD.md дополнен `emu_disp.cpp` + `emu_ioctl.cpp`.
Образ 33792 → 38400 Б, `dumpbin /imports` по-прежнему только
`ntoskrnl.exe` + `portcls.sys`, `./utest.sh` — 4 из 4 (desc 209, ctrl 123,
state 58, ioctl 171), `smoke.sys` и `sample.sys` линкуются.

### Железный прогон M2 (2026-11-04): старт на живой 0404 и три правки парсера

Драйвер установлен на живую 0404 (`kinstall.ps1`, тестовый сертификат,
testsigning уже был включён). Прогон закрыл половину вопросов этапа 4b и
поднял один новый.

**Что работает на железе.** `DriverEntry`/`AddDevice`/`StartDevice` — чисто:
layout-гвард M1 не сработал, `USBD_CreateHandle` + два чтения дескриптора
прошли (1832 байта, `wTotalLength` совпал между чтениями), парсер разобрал
устройство: 4 интерфейса (AC/render/capture/MIDI), 32 streaming-альта,
4 E-MU XU (0x0C..0x0F, коды 0xE301..0xE304), feature unit 0x04 (master =
mute only), 44100-192000 Гц. Control interface зарегистрирован и включён
(`pnputil /enum-interfaces`: «Включено»).

**Реконструкция фикстуры оказалась неверна в трёх местах** — реальный
дескриптор сначала был отвергнут (`STATUS_DEVICE_CONFIGURATION_ERROR`), hex
дамплен из ядра и разобран скриптом. Фиксы: (1) направление по
`bTerminalLink` было инвертировано — render-интерфейс ссылается на
INPUT_TERMINAL 0x01, capture на OUTPUT_TERMINAL 0x0A (UAC1-семантика «поток
входит через input-терминал»); (2) 16-битные альты 17/18 объявлены
FORMAT_TYPE_III — теперь помечаются `NonPcm` и пропускаются, а не роняют
устройство; (3) реальных альтов 34 против лимита 24 — `EMU_MAX_STREAM_ALTS`
поднят до 64. Фикстура `tests/descfixture.h` заменена реальными байтами
(1832, слово в слово), ожидания всех тестов переведены на живое устройство:
`utest.sh` — 4 из 4, 576 проверок. Утверждение RE_NOTES про скрытый class
байт (0xFF + стандартный subclass) подтвердилось точно.

**GET_CUR clock-rate пробник — вердикт получен 2026-11-05.** Первая
диагностика (0xC000000D = STATUS_INVALID_PARAMETER) была отказом USB-стека
ДО устройства: `_URB_CONTROL_TRANSFER` с `PipeHandle = NULL` требует флага
`USBD_DEFAULT_PIPE_TRANSFER` в TransferFlags (бит входит в
`VALID_TRANSFER_FLAGS_MASK`; SelectConfiguration для этого НЕ нужен —
именно он нужен для стриминга/альт, M3). После добавления флага (2.0.0.8)
пробник на железе отвечает `0xC0000001` (STATUS_UNSUCCESSFUL — URB ушёл на
устройство, отказ на/от шины) — согласуется с записью v1: firmware
CA0189 не отвечает на GET_CUR (stall). **Кэш состояния наполняется
только SET_CUR** — развилка этапов 2/3 закрыта окончательно. Вердикт
виден без kernel-трейса: драйвер зеркалирует исход пробника в
`Parameters\LastProbeStatus`/`LastProbeRateHz` (breadcrumb-канал,
пригодившийся ещё и потому, что DebugView 5.02 из winget не работает
как kernel-капчер на этой машине, а Driver Verifier — см. ниже —
блокирует kernel-записи в реестр из DriverEntry).

### SET-путь (2026-11-05): контракт, экзекьютор и приём железом

Критерий M2 требует, чтобы панель меняла rate/clock/S-PDIF/soft limit.
Блокер «нет трейса» снялся иначе: SET-сборщики уже были протестированы
байт-в-байт против v1-движка (`utest.sh ctrl`), а вердикт железа стал виден
через кэш состояния — принятый SET_CUR отражается в GET_STATE
(`ValidMask`/поле/`StateSequence`), отказ — в `LastOperationStatus`.

Контракт дополнен четырьмя кодами записи (0x802..0x805,
FILE_WRITE_ACCESS — SET отличается от query того же function-кода номером
кода), вход `EMU_SET_INPUT` (Size/Version/Value, 3 ULONG, C_ASSERT), ответ —
освежённый `EMU_STATE_INFO` (верdict и значение в одном roundtrip). Правила
classify: для SET — input >= `sizeof(EMU_SET_INPUT)` (новая панель может
дописывать хвост), output >= `sizeof(EMU_STATE_INFO)`; «не готов» по-прежнему
опережает проверки буфера.

В драйвере: `EmuUsbExecuteSet` — вся операция под StateLock (SET против
SET и SET против stop сериализованы; GET_STATE читатели ждут control-
transfer — миллисекунды — вместо половины применённого изменения).
Descriptor-veto до шины: rate, которого нет в дескрипторе, и SET для
отсутствующего юнита отказываются без roundtrip. Кэш дополнили
домен-проверки трёх байтовых контролов (clock 0/1, spdif 0/1/2,
softlimit 0/1) — правило то же: отказ не трогает значение и sequence.
`LastOperationStatus` живёт в контексте под тем же StateLock и
сбрасывается в release (операция принадлежит мёртвой сессии).

Пробник (`emu-probe set-rate/set-clock/set-spdif/set-softlimit`) —
замена панели для приёмки. **Результат на живой 0404:** SET_RATE 48000 и
96000 — приняты (rate в GET_STATE, sequence 1→2), SET_CLOCK_SOURCE 1 —
принят, SET_SOFT_LIMIT 1 — принят, SET_SPDIF_FORMAT 1 и 2 — приняты;
SET_RATE 44101 и SET_SPDIF_FORMAT 3 — отказы descriptor/domain-veto до
шины с нетронутым кэшем. Единственное отклонение: SET_SPDIF_FORMAT 0
("none") firmware отказывает (0xC0000001) — значение из v1-протокола,
которое железо не берёт; зафиксировано как деталь протокола.

Инфраструктурный факт того же вечера: kernel-образ драйвера НЕ
перегружается restart-device/disable/remove — только загрузкой ОС. Все
пост-бутные инсталлы меняли файл на диске, но исполнялся образ последней
загрузки; диагностировалось по ImplementedMask (0x03 у старого образа
против 0x3F у нового).

### M3-A (2026-11-05): SelectConfiguration успешен, select-interface
отказан — первая половина гейта стриминга

`EmuCapsFindInterfaceDescriptor` (двойной исходник, смещение 9-байтного
interface-дескриптора в блоке) + `EmuUsbSelectConfiguration`
(USBD_INTERFACE_LIST_ENTRY по всем четырём audio-интерфейсам на alt 0,
USBD_SelectConfigUrbAllocateAndBuild; URB и configuration handle живут в
контексте весь старт — interface info живёт внутри URB-аллокации).
**Железный вердикт: SelConfigStatus = 0 — SelectConfiguration прошёл на
живой 0404** (маркер EntryBread=0x15 подтвердил свежий образ).
Диагностический alt-проход (render/capture → их 48 кГц альты и обратно в
alt 0) остановился на первом шаге: SelRenderStatus = 0xC000000D. Кандидат
с лучшей аргументацией — одиночный entry без {NULL,NULL}-терминатора в
USBD_SelectInterfaceUrbAllocateAndBuild; правка — терминатор, проверка —
после следующей перезагрузки (образ живёт до неё).

Инфраструктурный закон цикла разработки, оформленный окончательно:
**kernel-образ драйвера выгружается только загрузкой ОС** —
restart-device, disable, remove и даже удаление импортов со службой
меняют привязку и файл на диске, но исполняемый образ живёт до перезагрузки
(диагностика: маркер DriverEntry EntryBread со значением на сборку
против устаревших значений). kinstall.ps1 теперь выполняет
remove-device → sc stop → удаление импортов → add-driver → scan-devices:
привязка и диск обновляются, образ — после перезагрузки.

**Новый блокер: `CreateFile` на interface symbolic link даёт
ERROR_FILE_NOT_FOUND** при включённом интерфейсе и работающем драйвере. Это
`IRP_MJ_CREATE` на адаптерном FDO, и его отвечает KS-диспетчер PortCls:
create item'ов под пустое имя файла нет (subdevice'ы не регистрировались),
ожидаемый статус — `STATUS_OBJECT_NAME_NOT_FOUND`. Утверждение плана
«control-PDO на первом этапе не нужно» опровергнуто железом: следующий шаг
(4c) — собственный контрольный device object (raw-PDO либо control-DO) с
регистрацией интерфейса на нём и собственными CREATE/CLOSE/DEVICE_CONTROL,
KS-трафик остаётся на FDO. Диагностика включённости теперь видна без
kernel-трейса: драйвер зеркалирует исход set-state в
`Parameters\ControlSetState`/`ControlSetEnabled` сервисного ключа.

**Практика прогона (подводные камни):** смена содержимого пакета требует
bump `DriverVer`, иначе pnputil пропускает импорт как дубликат и старый
бинарник остаётся в DriverStore; обновление пакета требует перезагрузки
(дважды за прогон). Debug Print Filter (`Session Manager\Debug Print
Filter\IHVDRIVER`) читается только при загрузке, маска — бит `1 << Level`
(уровни драйвера 2..5, маска 63); `DbgPrintEx`-капчура через DebugView 5.02
из winget оказалась ненадёжной (пустые `/l`-файлы), поэтому заменена
registry-breadcrumb'ом. Свежий трейс стартовавшего драйвера (caps-таблица,
hex) получить пока не удалось — канал настроить до следующего прогона.

### Этап 4c: контрольное устройство и первый IOCTL-roundtrip (2026-11-04)

Опровержение из прогона реализовано немедленно: DriverEntry создаёт
именованный device object `\Device\EmuUSBAudioCtl` (IoCreateDeviceSecure,
SDDL «SYS_ALL_ADM_ALL» — та же политика, что в INF для FDO) с симлинком
`\??\EmuUSBAudioCtl` (симлинк NT-имени), удаляется в DriverUnload до chain-вызова PortCls.
Панель открывает `\\.\EmuUSBAudioCtl`; CREATE/CLOSE/CLEANUP этого DO
обрабатывает драйвер сам, DEVICE_CONTROL — по тому же контракту, а FDO
дискриминируется по DeviceType (FILE_DEVICE_SOUND против
FILE_DEVICE_UNKNOWN) плюс сигнатуре первого ULONG расширения — на FDO-пути
всё чужое по-прежнему уходит `PcDispatchIrp`. Ответ собирается из
глобального указателя на текущий стартовавший экземпляр (публикуется в
конце старта, снимается в stop/remove): контрольный путь держит
ObReferenceObject на FDO и его remove lock, поэтому panel handle на
контрольном устройстве не может пережить контекст, из которого читает —
заполнение по-прежнему под StateLock. PDO-регистрация интерфейса,
enable/disable и breadcrumb из этапа 4b удалены как мёртвые (секция
`.Interfaces` из INF тоже); пробник переключён на прямой CreateFile
(`driver/tools/emu_probe.c`).

**Результат на железе: `emu-probe: PASS`.** `GET_CAPS` вернул таблицу
живого дескриптора (16 render + 16 capture альтов, 4 канала, 6 дискретных
частот, юниты 0x0C-0x0F, monitor/metering — EMU_NO_VALUE, feature unit
0x04 master 0x01, status EP 0x83), `GET_STATE` — все четыре контрола
"unknown" с `pnp state = started` и счётчиками start 1/stop 0/remove 0.
ImplementedMask (get-caps, get-state) и SupportedMask (все шесть) расходятся
ровно так, как требует контракт. Первый IOCTL-roundtrip драйвера с
user-mode состоялся.

### Ksoak M2 (2026-11-05): 50 + 10 циклов PnP на живой машине

`ksoak.ps1 -Cycles 50 -RemoveCycles 10` (с исправленным поиском devnode —
тот же wildcard-баг, что был в kinstall): 50/50 disable/enable по ~0.6 с
за цикл, ни одного медленного цикла (признак зависшего remove lock от-
сутствует), 10/10 remove/re-add, ни одной ошибки emuusbaudio/portcls в
System-логе, пробник контрольного устройства — PASS после соака. Попутно
это первый прогон StateLock-пути: 60 полных стартов/стопов с публикацией
и снятием глобального указателя.

Замечание соака: nonpaged pool вырос на ~1.1 МБ за прогон, но показания
не монотонны (средний диапазон колебаний на живой машине того же
порядка) — утечку из этого не вывести; детерминированный ответ даст
Driver Verifier (Special Pool, отдельная перезагрузка) на том же соаке.
Критерий M1 (50 циклов под Verifier) пока не выполнен.

### Ksoak под Driver Verifier (2026-11-05): критерий M1 закрыт

`verifier /standard /driver emuusbaudio.sys` (Special Pool, Force IRQL,
pool tracking, deadlock detection, DMA, DDI compliance), перезагрузка,
повторный `ksoak.ps1 -Cycles 50 -RemoveCycles 10`:

- 50/50 disable/enable по ~1.0 с (Verifier-накладные поверх 0.6 с), ни
  одного медленного цикла;
- 10/10 remove/re-add; ни одной ошибки emuusbaudio/portcls в System;
- ни одного BSOD и ни одного дампа в C:\Windows\Minidump;
- пробник контрольного устройства — PASS после соака;
- pool-дельта снова не монотонна (+1.7 МБ в тех же границах шума).

Открытый вопрос «интерфейс не включается» из этапа 4b закрыт как ложный:
`pnputil /enum-interfaces` показывал «Включено» с самого первого пуска,
настоящий отказ происходил в KS на CREATE (см. этап 4c).

**Два системных факта прогона, найденных breadcrumb-инструментацией:**

1. **Driver Verifier блокирует kernel-записи в реестр из DriverEntry.**
   Breadcrumb-значения (`EntryBread` из DriverEntry, `EmuCtlBread` через
   ZwCreateKey) появились ТОЛЬКО после `verifier /reset` + перезагрузка; в
   2.0.0.6 они писались до включения Verifier. Пока Verifier включён,
   breadcrumb-канал диагностики не работает — все «пропажи» значений за
   вечер объясняются этим, а не багами драйвера.
2. **pnputil /add-driver /install не перевязывает devnode на новую версию
   того же INF.** Инсталлы 2.0.0.10-2.0.0.14 не исполнялись вообще —
   устройство оставалось на предыдущем импорте. kinstall.ps1 теперь снимает
   все импорты `openemu-ks.inf` перед установкой (заодно исправлен поиск
   devnode: паттерн без `*` не совпадал с InstanceId с серийным суффиксом).
   Диагностика версий: DEVPKEY_Device_DriverVersion, а не вывод enum-drivers.

Дальше: SET_CUR по трейсу (флаг USBD_DEFAULT_PIPE_TRANSFER уже разблоки-
ровал EP0-class-запросы — SelectConfiguration остаётся для стриминга/альт,
M3) и ksoak под Verifier снова.
Примечание к пробам 4b: root-причина «интерфейс не включается» была
ложной — pnputil показывал «Включено» с самого начала, настоящий отказ
происходил в KS на CREATE.

## Тесты (обязательные)

1. Parser: реальные дескрипторы всех моделей, malformed lengths, лишние
   endpoint'ы, превышение размеров массивов. **Часть закрыта 2026-11-04**
   (`driver/utest.sh desc`: malformed lengths, лишние/переставленные endpoint'ы,
   переполнение массивов, оба XU-layout, поиск alt по частоте и направлениям
   (в том числе 4ch@192 кГц = NULL) и список частот с дедупом и усечением —
   209 проверок, `RESULT: PASS`);
   реальные дескрипторы всех моделей — после `emu-dump -o docs/descriptors`.
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
9. Раскладка class-запросов EP0: восемь setup-байтов каждого `SET_CUR`/`GET_CUR`
   против v1-движка, границы полей и кодировки значений. **Закрыто 2026-11-04**
   (`driver/utest.sh ctrl`: 123 проверки, `RESULT: PASS`); принятие этих байтов
   hardware'ом — из того же M1-трейса.
10. Кэш состояния: то, против чего говорит дескриптор, не записывается и не
    затирает уже известное; до первого подтверждения каждое поле — «неизвестно»,
    а не ноль; счётчик принятых заметок двигают только принятые.
    **Закрыто 2026-11-04** (`driver/utest.sh state`: 58 проверок,
    `RESULT: PASS`); наполняется ли кэш чтениями, решает ответ `GET_CUR` на
    живом устройстве.
11. Контракт панели: чужой control code уходит PortCls нетронутым, refusal для
    «не готово»/несовместимых буферов, `Size`/`Version`, несовпадение
    `SupportedMask` и `ImplementedMask`, `ValidMask` против `EMU_NO_VALUE`,
    смещения и размеры обоих ответов, правило «когда экземпляр отвечает»
    (`EmuIoctlIsDeviceUsable`). **Закрыто 2026-11-04**
    (`driver/utest.sh ioctl`: 171 проверка, `RESULT: PASS`); что панель реально
    получит по этим IOCTL, покажет живое устройство — обработчик и регистрация
    interface написаны (этап 4b), но ни один запрос по ним ещё не ходил.

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
  ntoskrnl/hal/wdm/libcntpr + portcls/stdunk/ksguid/ks + bufferoverflowk +
  usbd/usbdex + wdfdriverentry/wdfldr (KMDF 1.35). Ссылочно-неиспользуемые
  библиотеки образ не меняют: каркас M1 по-прежнему импортирует только
  `ntoskrnl.exe` и `portcls.sys`. Обязательная пара к `/GS`-либам —
  `-NODEFAULTLIB:LIBCMT.lib` + `BufferOverflowK.lib` (см. «Подготовка M2»),
  иначе driver-линк тонет в user-mode `gs_report`. Пути находятся сами
  (MSI-layout `Windows Kits\10`, либо NuGet `<root>/c/Include`);
  переопределение — `OED_VS/OED_MSVC/OED_KITS/OED_SDKVER/OED_WDFVER/OED_NTDDI`.
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
