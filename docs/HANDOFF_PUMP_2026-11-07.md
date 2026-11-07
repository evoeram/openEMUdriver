# Handoff: iso-насос — что узнали, что починили, что дальше

SPDX-License-Identifier: MIT
Дата: 2026-11-07, ночь. Сессия: вторая итерация реверса + три железных прогона.
Файлы-первоисточники: `ISOCH_SCHED.md` (планировщик), `ISOCH_PUMP.md` (наос +
completion + якорь, §9 = внесённые фиксы), `V2_PLAN.md` (журнал прогонов).

## 0. Одной строкой

Движок переписан с «8 слотов + воркер-поток» на вендорскую модель насоса
(FUN_000148A8): URB без данных не существует, re-arm — в completion (DPC),
один глобальный курсор +1/URB с якорем `cursor<=now → now+3` в каждой
постройке, feedback — 2 async URB с курсором +32. На железе за вечер
пойманы и закрыты три собственных бага этого переписывания (BSOD, дедлок
рукопожатия, фриз конвейера); текущая сборка собрана и протестирована
(utest 6/6), **последний фикс ещё не гонялся по железу**.

## 1. Хронология сессии (прогоны и что каждый доказал)

| Прогон | Что поставлено | Что случилось | Причина | Фикс |
|---|---|---|---|---|
| 1 (.71, прайм-насос) | переписанный движок | **BSOD 0x7F/0x8** (double fault) | неограниченная рекурсия насоса через инлайн-комплишн: слоты освобождаются так же быстро, как занимаются — пул не ограничивает цепочку, DPC-стек переполняется | single-flight `Pumping` (CAS), лимит синхронного спина 16, кадровый IRP учтён в `Pending` |
| 2 (фиксы 1) | + барьеры | бсода нет, но **плееры не могут начать воспроизведение**; `FillSeen` отсутствует = 0 URB | дедлок рукопожатия WaveRT: движок пишет по нашим notification-событиям, события — только от URB-completion, а URB без данных не существует (старый прайм был скрытым бутстрапом); плюс первый `SetWritePacket(0)` отклонялся как «late» (`ahead=0<1`) | первый релиз принимается (`m_LastOsWriteAbs=k+1`, `SwrFirst`); бутстрап-сигнал нотификаций при RUN (`NotifKick`) |
| 3 (фиксы 2) | + рукопожатие | плеер стартует (RUN входит), но **тишина**; stats: Submits=Completes=16 data + 10 fb, ноль ошибок, Running=1 — **все URB завершены, 17-й не сабмитился**, филлы не падали, гейт не блокировался | `EmuIsochBusFrame` делал `IoCallDriver` кадрового запроса изнутри completion-DPC: на USB3 запрос не отвечает инлайн (в отличие от USB2 вендора) → стек клинит (наш DPC не может вернуться, пока внутри него ждёт новый запрос) | на DISPATCH якорь = только interrupt-time mapping; реальный кадровый запрос — только на PASSIVE-входе насоса (до PumpLock) |
| 4 (фикс 3) | + mapping-якорь | **не проверено** — сборка готова | — | — |

Ключевой вывод прогонов 1–3: **никаких submits/ожиданий изнутри собственного
completion-DPC** — перезарядка iso-URB из DPC работает доказанно (8 подряд в
каждой сессии), а всё остальное (кадровые запросы, ожидания) — нет.

## 2. Вендорская модель (истина по двум итерациям реверса)

Полное описание — `ISOCH_SCHED.md` и `ISOCH_PUMP.md`. Суть:

- Насос `FUN_000148A8` — конечный цикл, **ничего не ждёт**; три событийных
  входа: KS-запись в кольцо, completion, старт/резюм.
- Гейт `fill < need → break`: **URB без данных не существует**, при пустом
  кольце провод молчит; при появлении данных строитель ренчорится
  (`cursor<=now → now+3`, адрес 0x4DBE) — поток возобновляется с текущего
  кадра +3.
- Копия кольцо→URB — в момент постройки (0x4B6B), под спинлоком; после
  сабмита буфер заморожен.
- Поля URB: 1 URB = 1 мс = 2 iso-пакета × 144 Б (мкф 0 и 4); StartFrame
  явный, +1 кадр за URB (трейс); TransferBufferLength/Length — фактические
  (недобор 144→138 в трейсе); Hdr.Length 0xB0; база IsoPacket 0x8C.
- Completion (`FUN_000162C8`→`FUN_00015C88`): статус из Irp+0x30; фатальные
  0xC0000120/0xC000009D/0xC000000E → recycle без resubmit; иначе recycle +
  немедленный насос из DPC. Счётчик+событие (+0x178/+0x180) — только для
  останова.
- Feedback: 2 iso IN URB × 4 Б, StartFrame = fbCursor+32 (дельта ровно 32 в
  трейсе), fbCursor ренчорится `<=now → now+3`, resubmit из своего
  completion (`FUN_0001466C`), который обновляет rate-аккумулятор data-строка
  (0x1A4/0x1A0) — у нас это M5.
- Старт: только `SYNC_RESET_PIPE_AND_CLEAR_STALL` перед ожиданием данных;
  USBD.SYS — ровно 2 импорта, остальное — сырые URB; {0x80,X} при ресинке —
  программный узел, без USB.

## 3. Наша текущая архитектура (после фиксов)

`Emu/emu_isoch.{h,cpp}` (переписан), `Emu/emu_wave.{h,cpp}` (колбэки):

- **Насос** `EmuIsochPump(S, Ctx)` — любой IRQL, single-flight
  (`S->Pumping` CAS; вложенный вызов из инлайн-комплишна сразу выходит,
  внешний цикл продолжает дренаж), сериализован `PumpLock`. Цикл: свободный
  слот → гейт данных → `EmuIsochBuildSlot` (филл + фактические длины,
  кумулятивные офсеты) → якорь (mapping) → `StartFrame = CursorFrame++`
  (+1/URB) → IoReuseIrp + IoSetCompletionRoutine → **сабмит ВНЕ спинлока** →
  при не-PENDING возврате только счётчики (касаться слота нельзя — вложенный
  насос мог его перевооружить).
- **Три входа насоса**: старт (`EmuIsochStartStream`, прайм), данные
  (`EmuIsochRenderDataReady` ← `SetWritePacket`), completion
  (`EmuIsochDataCompletion`: reap → насос).
- **Reap**: `InterlockedExchange(ARMED→FREE)` решает владельца харвеста;
  `Pending` — счётчик всех вооружённых URB (данные + feedback + кадровые
  запросы), ноль ставит `Idle` (останов ждёт дренаж ≤3 с, застрявшее = retention).
- **Кадровый источник**: `EmuIsochQueryFrameReal` — PASSIVE-only (пумп-вход,
  старт); `EmuIsochBusFrame` на DISPATCH — **только mapping**
  (`PaceOrigin/PaceBaseFrame/FramesPerMs`, калибровка 10 мс на старте,
  обновление каждым PASSIVE-запросом). Это осознанное отклонение от вендора
  (у него реальный запрос в каждой постройке — легально только на USB2-стеке).
- **Feedback**: 2 слота `Fb[2]`, перезарядка из своего completion с
  глубиной ≤4 (`FbRearmDepth`), `FbCursorFrame`: якорь `<=now → now+3`, затем
  `+=32`, `StartFrame = FbCursor`. Значения фидбека в счётчики (M5 их прочитает).
- **WaveRT-сторона**: `FillUsbPacket` (возвращает реально упакованные кадры,
  0 = отказ; без ожиданий, DPC-безопасен, одиншоты реестра только на PASSIVE),
  `FramesAvailable` (гейт: выпущено OS минус позиция; без packet-модели =
  безлимит), `SetWritePacket` принимает первый релиз сессии и пинает насос,
  `KickNotifications` при RUN (бутстрап рукопожатия).
- **Останов**: retire под PumpLock (Quit=1, Active=FALSE) → ABORT_PIPE обоих
  пайпов → дренаж `Pending` → free; застрявшее — retention.

Осознанные отличия от вендора: mapping-якорь на DPC (см. выше), гейт по
`FramesAvailable` вместо кольца KS, статистика через stats IOCTL (ABI
заморожен — 15 ULONG на направление, не менять).

## 4. Инструментарий диагностики

- `driver/tools/stats_dump.c` → `stats_dump.exe` — читает
  `EMU_IOCTL_TEST_STREAM_STATS` из `\\.\EmuUSBAudioCtl` (**запуск через UAC**,
  устройство admin-only). Счётчики живут после STOP до следующего старта.
- `driver/tools/pump_probe.c` → `pump_probe.exe` — воспроизведение насоса
  без плеера: TEST_STREAM_START(48000) → 8×500 мс поллинга → STOP.
  Здоровый насос: capture (и render в pattern-режиме) растёт бесконечно.
  Запуск: `C:\dev\EMU\emu_pump.cmd` (UAC).
- **Breadcrumb'ы** (`HKLM\SYSTEM\CurrentControlSet\Services\EmuUSBAudio\Parameters`,
  читаются без админа) — таблица имён:
  - сессия/старт: `StateEnter`, `WaveStartStatus`, `ProbeArm`, `FillSet`,
    `NotifyFrames` ((Count<<20)|Frames при RUN; 0 = без packet-модели),
    `NotifKick`, `TestPipe0/1`, `TestFb0/1`, `CalFramesPerMs`, `TestFrame0/1`,
    `RateSetEnter/StreamRateSet/MuteSetStatus/VolSetStatus`
  - насос (флаш при STOP): `PumpUnderruns`(/C), `FqOk`, `FqFloat`, `FqFail`,
    `FqBusy`, `AnchorFires`, `NowLast`, `CursorLast`, `PumpQuitExit`,
    `PumpPoolExit`, `PumpGateExit`, `PumpBuildExit`
  - волна (флаш при STOP): `FillCalls`, `ArmTicks`, `Underruns`, `SwrLate`,
    `SwrOver`, `FillRejectReason` (1=validation, 2=spans, 3=pack),
    `FillRejectPos`, `FillBufferBytes`; one-shot'ы `FillSeen`,
    `FillRejected`, `SwrFirst` (PASSIVE-only — их отсутствие при нулевых
    счётчиках означает «не было PASSIVE-вызова», не «не было вызова»)
- Дампы BSOD: `C:\Windows\Minidump\` (110726-9031-01.dmp = 0x7F/8 прогон 1),
  kd.exe из WDK; чтение требует админа (копировать elevated-командой).
- USBPcap установлен, но не привязан к хабу (сервиса нет) — проводная правда
  сейчас только через счётчики/трейс DbgPrint (`khw-trace.ps1`).

## 5. Что доказано железом

- URB уходят и завершаются без единой ошибки: 16 data (2×144 Б, 1 мс, +1
  кадр/URB) + 10 feedback (4 Б, дельта 32) — прайм (PASSIVE) работает,
  перезарядка из DPC работает (8 подряд), Length/TBL без коротышей.
- Старт-цепочка (SET rate → mute/volume → select alt → RESET_PIPE) — success.
- Пайплайн 8×48 кадров покрывает расписание [now+3, now+11].
- BSOD-класс закрыт (прогон 2 без падений).

## 6. Открытые вопросы и следующие шаги (по порядку)

1. **Прогнать фикс 3 по железу** (сборка готова, НЕ установлена):
   `kinstall.ps1` → `emu_pump.cmd` → Submits/Completes должны расти
   бесконечно (capture самоподдерживается; render в pattern-режиме тоже).
2. Если счётчики растут, а звука нет — вопрос за DAC-lock: проверить
   фидбек на проводе (значения 16.16 в Fb-буферах сейчас идут только в
   счётчик Bytes — при необходимости добавить breadcrumb последнего
   фидбек-значения), XU clock rate (шлётся при каждом старте), settle
   время. Сравнить с `stock-tone.pcap`/`stock-trace.pcap` в `_analysis`.
3. Если фриз повторился — читать `Fq*`/`Pump*Exit`/`FillReject*`/`CursorLast`
   из Parameters после STOP: они покажут точное место (заклинивший кадровый
   запрос теперь единственный подозреваемый класс, и он исключён; следует
   ожидать чистого паттерна выходов).
4. Если плееры снова «не могут начать» — `NotifyFrames`/`SwrFirst`/`NotifKick`
   покажут, дошло ли рукопожатие.
5. Разобрать minidump 110726-9031-01 по стеку (подтверждение рекурсии) —
   опционально, класс уже закрыт фиксами.
6. Идея на потом (M5): feedback-значения → rate-аккумулятор data-строка, как
   у вендора (`FUN_000143D0` обновляет 0x1A4/0x1A0); сейчас режим строго
   48k/1 мс без дробных пакетов.

## 7. Состояние дерева

- Изменены: `Emu/emu_isoch.{h,cpp}` (переписан), `Emu/emu_wave.{h,cpp}`,
  `docs/ISOCH_PUMP.md` (§9), `docs/V2_PLAN.md` (3 записи), новые
  `driver/tools/{stats_dump,pump_probe}.c` (+ собранные exe рядом).
- `kbuild.sh` — BUILT; `utest.sh` — 6/6 (isoch-юнит починен: ntstrsafe.h
  под EMU_KERNEL).
- **В git не закоммичено.** Установка: admin PowerShell →
  `cd C:\dev\EMU\openEMUdriver\driver; .\kinstall.ps1`.
