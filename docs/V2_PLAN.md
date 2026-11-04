# openEMUdriver v2 — kernel-драйвер (WASAPI + ASIO)

Статус и карта работ. Продолжение отсюда = новая сессия начинается с раздела
«Текущее состояние».

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
  Пока ASIO активен — WASAPI-приложения получают «device busy» (как ASIO4ALL).
* Контролы — через IOCTL-интерфейс драйвера (панель и ASIO), драйвер шлёт
  EP0 SET_CUR по известному протоколу (docs/PROTOCOL.md).

## Текущее состояние (всё проверено)

1. **Тулчейн ядра готов и доказан**: MSVC 14.44 (Build Tools уже стоят) +
   WDK NuGet `10.0.26100.6584` (`C:\dev\tools\wdk`) + SDK NuGet
   `C:\dev\tools\sdkall`. Smoke-драйвер собирается и подписывается:
   `driver/emuusbaudio/build/smoke.sys` (kbuild.sh).
2. `driver/kbuild.sh` — сборочный harness: `-kernel -GS-`, порядок include'ов
   `km\crt → ucrt → um → shared → MSVC → km → shared` (важно! иначе
   `_CRTIMP_ALT` взрывается), libs: ntoskrnl/hal/wdm/libcntpr +
   portcls/stdunk/ksguid/ks + WDF kmdf 1.35.
3. `driver/emuusbaudio/` — скопированы исходники simpleaudiosample
   (Main/, Filters/, Inc/ из ref/Windows-driver-samples). Известные проблемы
   копии: `common.cpp` тянет несуществующий `hw.h` (сэмпл «поплыл» в апстриме)
   → **свой adapter.cpp переписать с нуля**, см. ниже. common.cpp/hw.cpp/
   savedata.cpp/ToneGenerator.cpp — не использовать.
4. Все знания по железу — в docs/RE_NOTES.md (включая кадровое планирование
   iso-трансферов и 4-байтовый feedback 16.16).

## План работ (порядок)

1. **Свой `adapter.cpp` (~600 строк)**: DriverEntry (PcInitializeAdapterDriver),
   AddDevice — настоящий USB FDO (IoCreateDevice + Attach + ioctl PDO;
   НЕ PcAddAdapterDevice-виртуалка), StartDevice: USBD_CreateConfigurationRequestEx
   → выбор конфигурации, затем создается PortCLS-адаптер
   (PcAddAdapterDevice с PDO от USB) и регистрируются эндпоинты
   (InstallEndpointFilters-аналог: speaker render + mic capture).
   IAdapterCommon реализовать без CHw/hw.h: Init/GetDeviceObject/
   SetWaveServiceGroup/Mixer* (локальное состояние)/InstallSubdevice/
   ConnectTopologies/InstallEndpointFilters/RemoveEndpointFilters/GetFilters/
   SetIdlePowerManagement/Cleanup (+ WriteEtwEvent — пустой).
2. **Форматы/таблицы**: `speakerwavtable.h`/`micarraywavtable.h` +
   `minipairs.h` → 24-бит в 4-байтовом контейнере (KSDATAFORMAT_WAVEFORMATEXTENSIBLE,
   wValidBitsPerSample=24), частоты 44.1/48/88.2/96/176.4/192k, render 2 канала
   (0404: альт 3–10), capture 2 канала. Драйвер выбирает USB-альт по частоте
   (таблица соответствий из v1: см. emu-dump вывод).
3. **USB-транспорт `usbdev.cpp` (~700 строк)**:
   - выбор интерфейса/альта (URB_SELECT_INTERFACE), iso-пайпы EP 0x01/0x82;
   - стрим: URB_ISOCH_TRANSFER с явным StartFrame (кадровое планирование как в
     v1; в ядре это надёжно и без WinUSB-квирков), 1 пакет/кадр, кольцо URB'ов
     4–8 в полёте; копирование WaveRT-буфера ↔ URB'ы (24-в-32 → 3-байтовые);
   - feedback EP 0x81 (4 байта 16.16) опционально отдельным URB-кольцом
     (в v1 сломалось параллельно с OUT — в ядре с явными StartFrame можно
     рассинхронизировать интервалы; если нет — номинальные пакеты);
   - EP0: SET_CUR/GET_CUR на XU (unit 0x0C..0x0F) + Feature Unit (unit 0x04).
4. **minwavert/minwavertstream**: убрать генератор тона; GetPosition — от
   USB-движка; Run/Pause → start/stop транспорта; перепаковка 24→32 — в движке.
5. **IOCTL-интерфейс**: device interface GUID (для панели/ASIO): set-rate,
   set-clock-source, set-spdif, soft-limit, get-state (из кэша — GET в fw нет).
6. **INF `openemu-ks.inf`**: bind whole-device (как openemu.inf), сервис
   EmuUSBAudio, Include=ks.inf,wdmaudio.inf Needs=KS.Registration,
   WDMAUDIO.Registration + AddReg wdmaud (образец: emaud.inf из пакета 1.30.07).
7. **Тест-подписание и загрузка**: `bcdedit /set testsigning on` + REBOOT
   (спросить пользователя!), затем devcon/pnputil install, DbgPrint-проверки.
8. **ASIO v2 (DirectKS)**: в openEMUdriver.asio добавить бэкенд: KsCreatePin на
   KS-фильтры (KSCATEGORY_REALTIME... нет: KSCATEGORY_AUDIO с именами Wave),
   буферы через IOCTL_KS_READ/WRITE или KSSTATE +allocator framing;
   бэкенд выбирается автоматически: если наш IOCTL-интерфейс есть → KS-режим.
9. **Панель v2**: тот же IOCTL-канал вместо WinUSB.

## Риски / заметки

- BSOD на живом железе: сначала Verifier + усиленные проверки параметров,
  нулевые данные; включать стрим только по шагам (логи DbgPrint).
- WaveRT позиция: софтверная (GetPosition от движка) — ок для v2.
- Если URB_ISOCH_TRANSFER с ContinueStream (USBD_START_ISO_TRANSFER_ASAP)
  будет капризничать как WinUSB-Asap — использовать точный StartFrame
  (USBD_QueryBusTime для текущего кадра).
- Не забыть: device в режиме «kernel» больше НЕ открывать WinUSB-папелью;
  oed-mode.ps1 дополнить третьим режимом `kernel`.

## Не забыть

- regsvr32 openEMUdriver.asio остаётся.
- Каталог `driver/emuusbaudio/build/` — мусор от компиляции.
- ref/Windows-driver-samples — референс (minwavert* — построчный референс
  для правок).
