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

## 3. Сборка v2 (smoke)

```bash
cd driver
./kbuild.sh emuusbaudio/build/smoke.sys emuusbaudio/smoke.c
```

Собирает что угодно из дерева драйвера (`Main/`, `Filters/`, `Utilities/`
добавляются списком исходников). Порядок `INCLUDE` зафиксирован:
`km\crt → ucrt → um → shared → MSVC\include → km → shared`; при другой
последовательности заголовки разваливаются на `_CRTIMP_ALT`.

Переопределения поиска (все необязательные):

| Переменная | Значение |
|---|---|
| `OED_VS` | каталог Visual Studio |
| `OED_MSVC` | версия инструментального набора, например `14.44.35207` |
| `OED_KITS` | корень Windows Kits (MSI- или NuGet-layout) |
| `OED_SDKVER` | версия заголовков, например `10.0.26100.0` |
| `OED_WDFVER` | версия KMDF, например `1.35` |
| `OED_NTDDI` | `NTDDI_VERSION`, по умолчанию `0x0A00000C` |

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

Каркас PortCLS (исходный SimpleAudioSample) компилируется целиком:

```bash
cd driver && ./kbuild.sh emuusbaudio/build/emuusbaudio.sys \
     emuusbaudio/Main/*.cpp emuusbaudio/Utilities/*.cpp emuusbaudio/Filters/*.cpp
```

Все 14 объектов собираются, линковка падает на 9 неразрешённых символах
user-mode `/GS`-поддержки из `LIBCMT` (`gs_support`/`gs_report` требуют
kernel32-импорты). Это осознанно оставлено на M1: план выводит оттуда
сэмпловую обвязку (`hw.cpp`, `savedata.cpp`, `ToneGenerator.cpp`), и вместе с
ней уйдёт зависимость от user-mode CRT. Подробности — раздел «Итог M0» в
`docs/V2_PLAN.md`.
