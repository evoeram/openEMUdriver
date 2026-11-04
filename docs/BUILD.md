# Сборка openEMUdriver

Инструкция для чистой машины. Два независимых контура: **v1** (user-mode,
MinGW-w64) и **v2** (kernel PortCLS, MSVC + WDK). v1 не зависит от WDK, v2 не
зависит от MinGW — можно поставить только то, что нужно.

---

## 1. Тулчейн v1 — MinGW-w64 (user-mode ASIO, панель, инструменты)

Нужен x86_64 GCC с UCRT-рантаймом. Проверено на WinLibs GCC 16.2.0
(posix-seh, mingw-w64 UCRT 14.0.0, r3), archive 276414382 байт.

```bash
# https://winlibs.com/ → «Win64 - x86_64 … UCRT – POSIX threads – release 3»
curl -L -o winlibs-mingw.zip \
  https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip
sha256sum winlibs-mingw.zip    # сверить с таблицей ниже
# распаковать куда угодно; внутри есть корень mingw64/
```

| Архив | SHA-256 |
|---|---|
| `winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip` | `3c69305081e4a647144339a936244fcf7d3d9e558319e1ad7be2cd2886d4b6a1` |

`build.sh` ищет компилятор в порядке: `$MINGW` → `$MINGW_HOME[/bin|/mingw64/bin]`
→ `/c/dev/tools/mingw64/*/bin` → `/c/msys64/mingw64/bin` → `/c/mingw64/bin` →
`g++` из `PATH`. Если ничего не найдено, скрипт печатает, что именно поставить.

```bash
export MINGW=/c/dev/tools/mingw/mingw64/bin   # только если g++ не в PATH
./build.sh
```

Артефакты: `bin/openEMUdriver.asio`, `bin/emu-dump.exe`, `bin/asio-host-test.exe`,
`bin/wasapi-test.exe`, `bin/openEMUPanel.exe`.

## 2. Тулчейн v2 — MSVC + Windows SDK + WDK (kernel)

```powershell
# компилятор ядра (MSVC 14.44.x) + Windows SDK
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

# WDK: km-заголовки, portcls.lib/stdunk.lib, KMDF 1.35
winget install --id Microsoft.WindowsWDK.10.0.26100
```

Проверенные версии: BuildTools 17.14.44 (MSVC 14.44.35207),
WDK 10.1.26100.6584. Компилятор ядра требует `-kernel`, поэтому сборка идёт
через `cl`/`link` напрямую, а не через MSBuild: готовые `.vcxproj` в
`driver/emuusbaudio/*` — из сэмпла и пока не подключены (M1).

Альтернатива MSI — NuGet-пакеты без админ-прав (layout `<root>/c/Include`,
`<root>/c/Lib`):

```bash
curl -L -o wdk.nupkg  https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/10.0.26100.6584/microsoft.windows.wdk.x64.10.0.26100.6584.nupkg
mkdir wdk && tar -xf wdk.nupkg -C wdk      # km+shared заголовки, km-либы, wdf
export OED_KITS=/c/dev/tools/wdk
```

Заголовки `ucrt`/`um`/`shared` в этом случае берются из компонента Windows SDK
(`Microsoft.VisualStudio.Component.Windows11SDK.26100`) — NuGet-пакет
`Microsoft.Windows.SDK.CPP.x64` содержит только библиотеки.

## 3. Сборка v2 (smoke и каркас M2)

```bash
cd driver
./kbuild.sh emuusbaudio/build/smoke.sys emuusbaudio/smoke.c
./kbuild.sh emuusbaudio/build/emuusbaudio.sys \
     emuusbaudio/Emu/emu_driver.cpp emuusbaudio/Emu/emu_pnp.cpp \
     emuusbaudio/Emu/emu_disp.cpp \
     emuusbaudio/Emu/emu_usb.cpp emuusbaudio/Emu/emu_desc.cpp \
     emuusbaudio/Emu/emu_ctrl.cpp emuusbaudio/Emu/emu_state.cpp \
     emuusbaudio/Emu/emu_ioctl.cpp
```

Собирает что угодно из дерева драйвера (`Main/`, `Filters/`, `Utilities/`
добавляются списком исходников). Порядок `INCLUDE` зафиксирован:
`km\crt → ucrt → um → shared → MSVC\include → km → shared`; при другой
последовательности заголовки разваливаются на `_CRTIMP_ALT`. Дерево `Emu/`
самодостаточно: `emu.h` виден из любого TU, линкуется без сэмпловой обвязки и
без KMDF — `dumpbin /imports` даёт только `ntoskrnl.exe` и `portcls.sys`,
включая сборку M2 целиком (`emu_usb.cpp`, `emu_desc.cpp`, `emu_ctrl.cpp`,
`emu_state.cpp`, `emu_ioctl.cpp`, `emu_disp.cpp`): `usbdex.lib` статический,
поэтому `USBD.SYS` в импортах не появляется. GUID контрольного интерфейса
определяется ровно в одном TU — `emu_pnp.cpp` первым включает `<initguid.h>`,
так что `DEFINE_GUID` из `Emu/emu_ioctl.h` разворачивается там в определение, а
в остальных TU и в user-mode тестах остаётся extern-объявлением.

Одна ловушка именно для C++-TU: `usbdex.lib` экспортирует плоские C-имена, а
`<usbdlib.h>` в C++ манглит объявления, и линк даёт LNK2019 на
`?USBD_CreateHandle@@YAJ…`. Лечится включением заголовка в `extern "C" { }`
(см. `Emu/emu.h`); `<usb.h>` выносить в `extern "C"` не нужно.

Библиотеки в линке: `ntoskrnl hal wdm libcntpr portcls stdunk ksguid ks
bufferoverflowk usbd usbdex wdfdriverentry wdfldr`. Ни `usbd.lib`, ни
`usbdex.lib`, ни KMDF-либы не попадают в образ, пока на них нет ссылок
(проверено на каркасе M1 — импорты те же), поэтому список общий для всех
целей. Две вещи, без которых kernel-линк не проходит:

* `-NODEFAULTLIB:LIBCMT.lib` + `BufferOverflowK.lib` — `/GS`-библиотеки
  (`usbdex.lib`, C++-объекты сэмпла) иначе резолвят `__report_gsfailure` из
  user-mode `LIBCMT!gs_report`, который тянет `__imp_QueryPerformanceCounter`,
  `__imp_TerminateProcess` и т. п. Это и есть стена M0 («полное дерево
  компилируется, но не линкуется, 9 неразрешённых символов»): теперь дерево
  линкуется целиком — `./kbuild.sh emuusbaudio/build/sample.sys
  emuusbaudio/Main/*.cpp emuusbaudio/Utilities/*.cpp
  emuusbaudio/Filters/*.cpp` → 91648 Б, импорты `ntoskrnl.exe`, `HAL.dll`,
  `portcls.sys`, user-mode-утечек нет.
* `usbd.lib` — import-библиотека **USBD.SYS** (легаси-хелперы
  `USBD_CreateConfigurationRequestEx`, `USBD_ParseConfigurationDescriptorEx`,
  `USBD_ValidateConfigurationDescriptor`, `USBD_QueryBusTime`);
  `usbdex.lib` — статическая реализация нового handle-API
  (`USBD_CreateHandle`, `USBD_UrbAllocate`, `USBD_IsochUrbAllocate`,
  `USBD_SelectConfigUrbAllocateAndBuild`, `USBD_SelectInterfaceUrbAllocateAndBuild`,
  `USBD_AssignUrbToIoStackLocation`, `USBD_CloseHandle`), которая находится по
  `GUID_USBD_INTERFACE` через PnP `IRP_MN_QUERY_INTERFACE` и не добавляет
  импортируемых модулей.

Переопределения поиска (все необязательные):

| Переменная | Значение |
|---|---|
| `OED_VS` | каталог Visual Studio |
| `OED_MSVC` | версия инструментального набора, например `14.44.35207` |
| `OED_KITS` | корень Windows Kits (MSI- или NuGet-layout) |
| `OED_SDKVER` | версия заголовков, например `10.0.26100.0` |
| `OED_WDFVER` | версия KMDF, например `1.35` |
| `OED_NTDDI` | `NTDDI_VERSION`, по умолчанию `0x0A00000C` |

Поиск инструментального набора вынесен в `driver/toolfound.sh`: его читают и
`kbuild.sh`, и `utest.sh`, так что user-mode тесты собираются теми же
найденными MSVC/WDK, что и драйвер.

### Юнит-тесты двойных исходников (плановый тест №1, EP0-запросы, кэш состояния, IOCTL-контракт)

Четыре исходника имеют двойное назначение: `-DEMU_KERNEL` берёт типы из
`<ntddk.h>` и код попадает в образ драйвера, без define те же байты читаются в
user-mode через `<windows.h>`/`<ntstatus.h>` (см. `Emu/emu_desc.h`).

* `Emu/emu_desc.cpp` — разбор configuration descriptor в `EMU_DEVICE_CAPS`
  (плановый тест №1) и вопросы к этой таблице (`EmuCapsFindAlt`/
  `EmuCapsSupportsRate`). Вызывается из `Emu/emu_usb.cpp`, поэтому входит в образ.
* `Emu/emu_ctrl.cpp` — сборка class-запросов EP0 для CA0189 (`SET_CUR`/`GET_CUR`
  по протоколу `docs/PROTOCOL.md`) и кодеков значений. В образ входит: его зовёт
  `Emu/emu_usb.cpp` (`EmuUsbSendControl` + пробник clock-rate в `StartDevice`).
* `Emu/emu_state.cpp` — кэш четырёх панельных контролов и правило «не записывать
  то, против чего говорит дескриптор». Тоже в образ: его зовёт тот же пробник, а
  `EMU_SETTINGS` лежит в `EMU_DEVICE_CONTEXT`.
* `Emu/emu_ioctl.cpp` — набор операций и раскладка ответов, которые панель
  читает (`Emu/emu_ioctl.h` — тот же заголовок с двух сторон), плюс правило
  `EmuIoctlIsDeviceUsable` («в каком состоянии экземпляр отвечает на запросы»).
  С этапа 4b входит в образ: его зовёт `Emu/emu_disp.cpp` — обработчик
  `IRP_MJ_DEVICE_CONTROL`, который всё, что не наши два кода, отдаёт
  `PcDispatchIrp` нетронутым.

```bash
cd driver && ./utest.sh             # все четыре модуля: собрать и запустить
cd driver && ./utest.sh -n          # только собрать, не запускать
cd driver && ./utest.sh desc        # только парсер
cd driver && ./utest.sh ctrl        # только EP0-запросы
cd driver && ./utest.sh state       # только кэш состояния
cd driver && ./utest.sh ioctl       # только контракт панели
```

`utest.sh` для каждого модуля делает два шага, и оба должны проходить:

1. модуль компилируется флагами `kbuild.sh` (`-kernel -GS- -W3 -WX`) — иначе
   «зелёный тест» может расходиться с кодом в `.sys`;
   финальная строка `KERNEL-COMPILE: emu_desc.cpp ok (kernel flags, /WX)`.
2. `tests/emu_<модуль>test.cpp` + тот же исходник линкуются в
   `emuusbaudio/build/emu_<модуль>test.exe` и запускаются; результат —
   `N checks, M failures` и `RESULT: PASS|FAIL`, итогом скрипт печатает
   `UTEST: K of K test binaries passed`.

Единица теста может линковать не один модуль: решение кэша зависит от таблиц,
которые парсер реально произвёл, поэтому `state` собирает `emu_state.cpp` **и**
`emu_desc.cpp` (оба проходят через шаг 1), а тест строит дескриптор, а не
заполняет `EMU_DEVICE_CAPS` руками. `ioctl` добавляет к этому
`emu_ioctl.cpp`: ответ панели собирается из тех же вопросов к таблице
(`EmuCapsAltCount`, `EmuCapsRateList`, `EmuCapsXuUnitId`), так что проверка
«маска и список частот следуют за дескриптором» остаётся проверкой парсера, а не
ожиданий теста.

Общие `CHECK`/`CHECK_EQ`/`CheckStatus`/`TestResult` — в `tests/testharness.h`;
счётчики и текущее имя случая (`g_checks`, `g_failures`, `g_case`) определяет
каждый тестовый TU, потому что бинарники запускаются отдельно.

Фикстура 0404 вынесена в `tests/descfixture.h` и собрана по замерам из
`docs/RE_NOTES.md`, а не снята с живого устройства; когда `emu-dump -o
docs/descriptors` (см. §6) даст raw hex, он заменит builder в одном месте, и
проверки по байтам станут строже сразу в двух тестах. Тест
`emu_ctrltest.cpp` проверяет восемь setup-байтов каждого запроса против раскладки
v1-движка (`src/emu_usb.cpp:311-360`), который уже работает на этом железе, —
проверять на нейросетевые догадки здесь нечего.

## 4. Подпись (отдельно от сборки)

Сборка не требует админ-прав и не трогает хранилище сертификатов. Подпись —
`driver/ksign.ps1`:

```powershell
cd driver
./ksign.ps1 -CreateCert -InstallCert          # один раз, от администратора
./ksign.ps1 -File emuusbaudio/build/smoke.sys -Verify
```

Чтобы тестовый драйвер вообще загрузился, на **тестовой** машине нужен режим
тестовой подписи — скрипт этого не делает и просит сделать осознанно:

```powershell
bcdedit /set testsigning on    # затем перезагрузка
```

Релизная ветка — attestation signing в Partner Center, тестовый сертификат для
неё не годится.

## 5. Референсы из `ref/` (не обязательны для сборки)

`ref/` вне репозитория (лицензии и размер); ни `build.sh`, ни `kbuild.sh` его не
читают — исходки драйвера уже лежат в `driver/emuusbaudio`.

```bash
tools/get-ref.sh samples      # microsoft/Windows-driver-samples (sparse: audio/simpleaudiosample)
tools/get-ref.sh emu-driver   # где взять LGPL-исходник E-MU CA0189 и почему его может не быть
tools/get-ref.sh check
```

## 6. Дампы дескрипторов

Дамп — это raw USB configuration descriptor (hex) плюс разобранная таблица
форматов и XU-юнитов. Файлы в `docs/descriptors/` — фикстуры для парсера
дескрипторов из M2 и для его юнит-тестов, чтобы парсер можно было тестировать
без подключённого устройства.

```bash
bin/emu-dump.exe -o docs/descriptors      # нужен подключённый E-MU (0404/0202/…)
```

Закоммиченных дампов пока нет: они снимаются только с живого железа.
Известные параметры 0404 (fw 8.04) зафиксированы в `docs/RE_NOTES.md`.

## 7. Быстрая проверка, что всё на месте

```bash
./build.sh                                   # v1: пять целей, финальная строка "OK: …"
cd driver && ./kbuild.sh emuusbaudio/build/smoke.sys emuusbaudio/smoke.c
grep -a "oed: smoke driver" emuusbaudio/build/smoke.sys     # маркер DbgPrint в PE
```

## 8. Что уже собирается из v2, а что нет

Каркас PortCLS (исходный SimpleAudioSample) компилируется и **линкуется**
целиком (с 2026-11-04, после фикса `/GS`-линии в §3):

```bash
cd driver && ./kbuild.sh emuusbaudio/build/sample.sys \
     emuusbaudio/Main/*.cpp emuusbaudio/Utilities/*.cpp emuusbaudio/Filters/*.cpp
# BUILT: …\sample.sys — 91648 Б; imports: ntoskrnl.exe, HAL.dll, portcls.sys
```

Цель называется `sample.sys`, а не `emuusbaudio.sys`, чтобы не перетирать бинарь
каркаса, который стоит на машине. Из M0 это закрытая стена («14 объектов, 9
неразрешённых символов user-mode `/GS`»): проблема была не в исходниках, а в
том, что харнес не давал линкеру kernel-реализацию `__report_gsfailure`.

Тем не менее в цель каркаса `Main/Utilities/Filters` не входят: M1 по плану —
PnP-скелет без аудио и без сэмпловой обвязки (`hw.cpp`, `savedata.cpp`,
`ToneGenerator.cpp`), и он линкуется отдельно (см. §3). Сэмпловое дерево —
материал для M4, и теперь известно, что оно собрано быть может.

## 9. Установка каркаса и приёмка M1 (нужны админ-права и 0404)

`driver/kinstall.ps1` — полный путь от исходников до связанного devnode'а:
сборка (опция `-Build`), стейджинг в `driver/package-ks/`, подпись бинаря,
`Inf2Cat` (signability-проверка + `.cat`), подпись каталога, снятие
конфликтующих пакетов v1/compat, `pnputil /add-driver /install` и статус
devnode'а/сервиса/событий System:

```powershell
cd driver
./kinstall.ps1 -Build          # установить каркас
./kinstall.ps1 -Uninstall      # вернуть v1 WinUSB (openemu.inf)
```

Без админ-прав скрипт проходит стейджинг, Inf2Cat и подпись и падает ровно на
`pnputil` (предупреждение заранее) — так пакет можно валидировать на любой
машине.

Приёмка этапа (50 disable/enable + remove/re-add под Driver Verifier) —
`driver/ksoak.ps1`: снимает nonpaged pool до/после, ловит зависшие циклы,
печатает ошибки по `emuusbaudio`/`portcls` из System:

```powershell
cd driver
./kinstall.ps1 -Build
verifier /standard /driver emuusbaudio.sys    # только на тестовой машине
shutdown /r /t 0                               # Verifier вооружается после перезагрузки
./ksoak.ps1 -Cycles 50 -RemoveCycles 10
```

`ksoak.ps1 -Verifier` только печатает эти команды и выходит — сам ни
`verifier`, ни `bcdedit`, ни перезагрузку не трогает. Прямые physical
replug'ы всё равно обязательны: `pnputil /remove-device` + `/scan-devices` не
воспроизводят surprise-remove посреди IO.

### Что должен показать первый прогон M2

Драйвер успевает поговорить с устройством до строки `started`, поэтому один
цикл старта при `Parameters\TraceLevel = 4` даёт (при 5 добавляется hex
дескриптора):

```
EmuUSBAudio: config <n>: <len> bytes, ac iface <i>, status ep 0x<ep>, ... Hz, flags ...
EmuUSBAudio: iface <i>: class 0x<..> subclass 0x<..> role <r>
EmuUSBAudio: alt iface <i> setting <a>: dir <d>, <ch> ch, <sf>/<bits> bit, data ep ...
EmuUSBAudio: XU unit 0x<id> code 0x<e30n> on iface <i> (layout <1|2>)
EmuUSBAudio: GET_CUR clock rate unit 0x<id>: index <n> = <hz> Hz, cached
EmuUSBAudio: started (<fdo>), start #1
```

Значимы две вещи. Первая: `config … iface/alt/XU` и hex при 5 — это настоящие
дескрипторы 0404, которых не хватает `docs/descriptors` (и фикстуре парсера).
Вторая: строка `GET_CUR clock rate`. Отвечает и значение проходит проверку по
дескриптору (`... = <hz> Hz, cached`) — кэш состояния наполняется чтениями, как
v1. Отвечает, но противоречит спискам частот — будет `WARN ... decodes to <hz>
Hz, which the descriptor does not carry; cache unchanged`, и это отдельная
зацепка (парсер или декодирование индекса). Нет (`… : 0xc0000182`, stall или
короткий data-stage) — значит зеркало собирается только из `SET_CUR`, и это надо
записать в `docs/PROTOCOL.md` до того, как панель будет полагаться на чтение;
до тех пор все четыре поля кэша читаются как `EMU_NO_VALUE`, то есть «не
известно», а не «ноль».

