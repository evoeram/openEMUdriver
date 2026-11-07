# emusba10.sys — насос рендера: поля URB, completion, якорь (итерация 2)

SPDX-License-Identifier: MIT
Дополнение к `ISOCH_SCHED.md`. Всё из дизассемблера amd64 (RVA = VA−0x10000)
и сверки с `ref57-stock.pcap` и нашим `emu_isoch.cpp`.

## 1. Структура насоса `FUN_000148A8` (RVA 0x48A8)

Насос **ничего не ждёт** — это конечный цикл, вызываемый событийно из трёх мест:

| Вызов | Контекст | Адрес вызова |
|---|---|---|
| KS-запись в кольцо | DPC под спинлоком кольца | `FUN_00013424` (0x13424): после `FUN_00012F20` → `FUN_000148A8(ring->0x28)` |
| Completion OUT-URB | DPC (IoCompletion) | `FUN_00015C88` (0x5C88), самый конец |
| Старт/резюм | PASSIVE | `FUN_00015278` (0x15278), ветка `!bVar2` |

Точное тело (все смещения проверены по дизасму 0x48A8–0x4CB9):

```
FUN_000148A8(stream):                       ; 0x48B8: if (stream->0x1C != 0) return  — только OUT
    spinlock(ring+0x58)                     ; 0x48CC
    if (stream->0x98 != 0) { ...режим якоря/ресинка, см. §5... }   ; 0x48D9..0x4A0C
    loop:                                   ; 0x4A0C
        fill = (ringWr - ringRd - 1) mod (ringCap+1)       ; 0x4A0C..0x4A30
        need = (0x1A0*0xA8 + 0x1A8)/(1000<<16) + 0x1A4*0xA8 ; 0x4A37..0x4A5B (магия 0x10624DD3, >>22)
        if (fill < need) break               ; 0x4A60  ← ПОРОГ: данных меньше 1 URB → СТОП
        req = pop(free_list 0x100)           ; 0x4A80  под спинлоком 0x118
        if (!req) break                      ; 0x4A9E  ← пул исчерпан (8 в полёте) → СТОП
        FUN_00014CD4(stream, req)            ; 0x4AAA  построить URB + СКОПИРОВАТЬ ДАННЫЕ (см. §2)
        req->0x3C = 0
        for i in 0..0xA8-1:                  ; 0x4AC8..0x4BB0
            acc = 0x1A8 + 0x1A0; 0x1A8 = acc mod (1000<<16)
            want = (acc/(1000<<16) + 0x1A4)*0x19C + 0x1AC     ; целевых байт
            if (want > req->0x50): 0x1AC = want-0x50; want = 0x50  ; clamp на MPS
            IsoPacket[i].Offset = (i==0) ? 0 : Len[i-1]+Off[i-1]  ; 0x4B24..0x4B3E
            got = FUN_000130D0(ring, req->buf + Off[i], want/0x19C, 0) ; 0x4B6B КОПИЯ СЕЙЧАС
            IsoPacket[i].Length = got * 0x19C                 ; 0x4B83
            0x1AC += Len[i] - want                            ; 0x4B93 перенос остатка
            req->0x3C += Len[i]
        urb->0x24 = req->0x3C                ; 0x4BBD  TransferBufferLength = Σ фактических длин
        if (stream->0x20 == 2) {             ; 0x4BC0  RUNNING
            KeInitializeEvent(stream+0x180)  ; 0x4BCA  (сброс «очередь пуста»)
            InterlockedIncrement(stream+0x178)                ; 0x4BDC
            push(pending 0x150, req)         ; 0x4BE4..0x4C2B под спинлоком 0x168
            FUN_00019234(dev, urb, irp, FUN_000162C8, req)    ; 0x4C49 САБМИТ
        } else {
            push(standby 0x128, req)         ; 0x4C53  пауза: URB построен, ждёт резюма
        }
    spinlock_release(ring+0x58)              ; 0x4CA5
```

**Ответы на подвопросы:**

- **Ждёт ли насос чего-то?** Нет: ни событий, ни таймеров, ни KsPinAttemptProcessing
  внутри цикла. Это pure worker: «пока есть данные И свободный req — строй и сабмить».
- **Когда копируются данные?** В момент построения URB, внутри цикла пакетов
  (0x4B6B), под спинлоком кольца, в контексте вызвавшего (KS-write DPC /
  completion DPC / старт PASSIVE). До вызова насоса данных в URB-буфере нет.
- **Глубина опережения**: (а) по данным — насос осушает кольцо до конца: всё,
  что движок записал в кольцо, немедленно уезжает в URB-буферы (буферы пула
  8 × ~298 Б = максимум ~8 мс аудио вне кольца); (б) по времени — StartFrame'ы
  в полёте занимают окно [now, now+8] (8 req × 1 кадр), первый строится на
  now+3. Дальше 8 URB вперёд насос не уходит — упирается в пул.
- **Пустое кольцо**: насос просто выходит на `fill < need` (0x4A60). **Никаких
  нулевых пакетов, никаких заглушек, никаких URB вообще.** Провод замолкает
  полностью (HCD нечего слать), а при появлении данных строитель
  `FUN_00014CD4` ренчорится: `if (cursor <= frame_now) cursor = frame_now+3`
  (0x4DBE..0x4DC9) — поток возобновляется с текущего кадра +3. Это и есть
  вендорский ответ на «прайм с нулями»: **URB без данных за ним не существует.**

## 2. Заполнение буфера: застывает ли после сабмита?

**Да, застывает навсегда.** Буфер req (+0x30, выделен один раз в
`FUN_00014ED0`/`FUN_000150A8`) заполняется `FUN_000130D0` только в момент
построения; после `FUN_00019234` (IoReuseIrp + IoCallDriver) драйвер в этот
буфер больше не пишет — ни в completion, ни где-либо ещё (греп по записям в
req->0x30: только строители). Перезаполнить «уже очередный» URB нельзя — и
вендору это не нужно, потому что URB не сабмитится, пока данные за ним не
лежат в кольце (порог `fill >= need`). Resync-запись `dev->0x4FC = cursor-6`
(0x4D98) — это штамп для мониторинга, не данные.

## 3. Поля URB: вендор `FUN_00014CD4` против нашего `EmuIsochArmUrb`

| Поле | Вендор (0x4CD4, дизасм 0x4CD4–0x4EA4) | Наш `ArmUrb` | Вердикт |
|---|---|---|---|
| Hdr.Length | `N*0xC + 0x98` = **0xB0** (на 1 дескриптор больше нужного 0x8C+N*0xC=0xA4) | `sizeof(_URB_ISOCH_TRANSFER)+1*12` = **0xA4** | Оба ≥ минимума; можно оставить |
| Hdr.Function | 0x000A | 0x000A | ✓ |
| PipeHandle | stream+0x78 | S->Pipe | ✓ |
| TransferFlags | OUT: **0**; IN: **3** (IN\|SHORT_TRANSFER_OK); бит ASAP (0x4) снимается явно (0x4D55 `and …,~4`) при req->0x48==0 | OUT: `USBD_TRANSFER_DIRECTION_OUT` (0); IN: IN\|SHORT | ✓ (ASAP не используется ни там, ни там) |
| TransferBuffer | req->0x30 (пул-буфер req, живёт вечно) | S->Buffer + slot*SlotBytes | ✓ эквивалент |
| TransferBufferLength | **Σ фактических длин пакетов** (0x4BBD), может быть < 288 при недоборе кольца | фиксированные `packetBytes` (288) | **разница**: вендор честно уменьшает при underrun |
| TransferBufferMDL | 0 (0x4D50 — не пишется, memset) | не задаём | ✓ |
| StartFrame | **кадры (1 мс)**: режим 0x98==0: GET_CURRENT_FRAME_NUMBER, `if (cursor 0x94 <= frame) cursor = frame+3` (0x4DC6), `StartFrame = cursor; cursor += 1` (0x4DD5) | `Slot->NextFrame` (base+i, затем шаг **+8 на слот**), re-анкор **только на старте** | **главная разница, см. ниже** |
| NumberOfPackets | req->0x4C = 0xA8 = 2 | 2 | ✓ |
| IsoPacket[i].Offset | кумулятивный **байтовый** офсет: 0, Len[0]+Off[0] (0x4B29..0x4B3E) | `p * (packetBytes/2)` = 0, 144 | ✓ |
| IsoPacket[i].Length | `got*frameBytes` — фактический недобор (144 / 138) | фикс 144 | **разница** (underrun) |
| База IsoPacket | **0x8C** = современная (StartFrame@0x80, NumPkts@0x84, ErrorCount@0x88, IsoPacket@0x8C) — примечание в ISOCH_SCHED.md §2 исправлено | 0x8C (WDK) | ✓ |
| Повторное использование | один пул FIFO, req безликий; IRP `IoReuseIrp` каждый сабмит | 8 слотов, каждый со своим IRP/URB (`USBD_IsochUrbAllocate`) | см. §7 |

**Структурная разница №1 — курсор.** У вендора один глобальный курсор кадров
на поток: каждый строитель каждый раз сверяет его с текущим кадром шины и
при отставании прыгает на `now+3`. У нас 8 независимых слотов, каждый после
прайма слепо шагает на +8 и **никогда не сверяется с шиной**: один поздний
воркер/затык — и расписание навсегда уезжает (слоты «догоняют» пачкой или
шлют в прошлое). Это видно и в трейсе: непрерывность StartFrame там держится
именно якорем в строителе.

**Структурная разница №2 — прайм.** Вендор при старте OUT не строит ни одного
URB (`FUN_00015278` для OUT идёт в `FUN_000148A8` сразу): URB появляется,
только когда в кольце ≥ 1 URB данных. Наш прайм шлёт 8 URB немедленно
(FillRender при пустом кольце = нули/что есть) — первые ~8+3 мс провода несут
мусор/тишину и, что важнее, стартуют расписание до начала реального потока.

## 4. Completion и перезарядка

`FUN_000162C8` (0x162C8, completion-рутина, регистрируется в стек-локации
`FUN_00019234` с Control=0xE0 — вызов на SUCCESS/ERROR/CANCEL):

```
FUN_000162C8(dev, irp, req):  FUN_00015C88(req->0x18 (stream), req); return STATUS_MORE_PROCESSING_REQUIRED (0xC0000016)
FUN_00015C88(stream, req):
    status = FUN_00015AA0(stream, req)      ; → Irp->IoStatus.Status (IRP+0x30, 0x5C88+decomp)
                                            ;   OUT: stream->0xB0 += req->0x3C (счётчик байт) независимо от статуса
                                            ;   IN: обход IsoPacket[].Status, подрезка «hyhuang»-тишины и т.п.
    fatal = (status == 0xC0000120 /*CANCELLED*/ || 0xC000009D || 0xC000000E)
    if (!fatal && stream->0x20 == 1) → recycle только
    else OUT (req->0x44 == 0):
        unlink из pending 0x150 (спинлок 0x168); 0x178--; if (0x178==0) KeSetEvent(0x180)
        if (req->0x54 & 1) FUN_00013718(ring, req)   ; нотификация позиции: ring vtbl(+0xA8)(ring+0xA0, 0)
        recycle → free list 0x100 (спинлок 0x118)
        FUN_000148A8(stream)                ; ← НЕМЕДЛЕННЫЙ ре-памп из completion
    else IN: FUN_00012F20(ring, packet data, frames) → rebuild FUN_00014CD4 → resubmit (самоперезапуск)
```

- **IRQL**: completion исполняется стеком USB в DPC ⇒ `DISPATCH_LEVEL`.
  Насос под спинлоками, memcpy, `IoReuseIrp`, `IoCallDriver` — всё легально на
  DPC. **Перезарядка следующего URB происходит внутри completion без единого
  ожидания** — зазор между завершением и следующим сабмитом ~микросекунды.
  У нас же: completion (DPC) → `KeSetEvent` → пробуждение воркера
  (PASSIVE, планировщик) → ArmUrb → IoCallDriver. Джиттер этой цепочки —
  единственный источник «отставания» расписания; вендору он не знаком.
- «Как насос узнаёт о завершении» — никак не узнаёт: completion **сам** зовёт
  насос. Событие +0x180/счётчик +0x178 нужны только для останова («все URB
  вышли»), не для метронома.

## 5. Синхро-якорь: ctx+0x70/0x74 и GET Id 0x10002

- RUN-переход пина (`FUN_0003BAEC`, 0x3BAEC): если `ctx->0x70 != 0` →
  `FUN_00019480(dev@ctx+0x30)` → `ctx->0x74 = frame + 3`; затем
  `FUN_000134BC(streams, ctx->0x70, ctx->0x74)`.
- `FUN_00015278(data, useExplicit, frame)`: `stream->0x98 = useExplicit`;
  курсор 0x94 = `useExplicit ? frame : GET_CURRENT_FRAME_NUMBER + 3`
  (0x4D59-ветка из дизасма 0x4CD4-функции... точнее: FUN_00015278 @0x5278,
  `FUN_00019480(stream->0x60); 0x94 += 3` при useExplicit==0).
- **Проверка `cursor <= frame → cursor = frame+3` живёт в КАЖДОЙ постройке**:
  data-строитель `FUN_00014CD4` @ **0x4DBE–0x4DC9** (курсор 0x94, ветка
  0x98==0), sync/feedback-строитель `FUN_00013E98` @ **0x3F1B–0x3F2A**
  (курсор 0x88). То есть якорь — это не стартовое действие, а инвариант
  цикла: любой простой автоматически переносит расписание в будущее.
- `GET Id 0x10002` возвращает `frame+3` — тот же маржин, что и в пине.
- Режим `stream->0x98 != 0` (явный кадр от панели): насос включает блок
  ресинка 0x48D9..0x4A0C: штамп `dev->0x4FC = cursor-6` (пишет строитель
  0x4D98), в насосе: `gap = stamp - cursor`; gap>100 → разовая запись
  контрола {0x80, X} через `FUN_00016338` (**программная** установка узла,
  не USB); gap>20 → **ресинк**: `skip = fill % (rate/1000)`;
  `FUN_000130D0(ring, NULL, skip, 0)` (выбросить неполный миллисекундный
  хвост), `cursor = stamp - 20`; gap≤20 → счётчик 0xA4, после 50 задержек и
  gap<15 → повторный {0x80, X}. Для штатного стока (панель не ставила
  0x10002) блок выключен.

## 6. Что ещё касается устройства между START и первым пакетом

- **Только** `URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL` (0x1E, Length 0x28,
  `FUN_00019B20` 0x9B20) на data-пайп перед праймом. Ни EP0, ни XU, ни
  SET_INTERFACE (SET_INTERFACE — раньше, в смене альта `FUN_0002898C`/`FUN_0002826C`).
- Interrupt-статусный EP обслуживается параллельно и всегда:
  `FUN_0002A4DC` (0x2A4DC, ctor) + `FUN_0002A7B0` (0x2A7B0, пампер):
  **16 URB** `BULK_OR_INTERRUPT_TRANSFER` (Function 9, Length 0x80, TF=3) на
  статусный пайп, completion `FUN_00012DAC` (0x12DAC) — recycle + resubmit.
  К расписанию рендера отношения не имеет.
- **Импорты USBD.SYS — ровно два**: `USBD_CreateConfigurationRequestEx`,
  `USBD_ParseConfigurationDescriptorEx`. Всё остальное — сырые URB через
  `IOCTL_INTERNAL_USB_SUBMIT_URB` (0x220003): CONTROL_TRANSFER_EX (0x0B),
  SELECT_INTERFACE (0x01), GET_CURRENT_FRAME_NUMBER (0x07, Length 0x20),
  SYNC_RESET_PIPE_AND_CLEAR_STALL (0x1E), ISOCH (0x0A), ABORT (0x02).
  `USBD_RegisterIsochBuffer`/`USBD_IsochUrbAllocate` форк не зовёт (их тогда
  не существовало); интерфейс-информация берётся из SELECT_INTERFACE-URB
  (кеш `FUN_00019180` — просто поиск в сохранённом массиве пайпов).
- Feedback — часть iso-планирования (sync-стрим §6 первого отчёта): 2 URB,
  StartFrame курсор +32 с якорем, resubmit из своего completion `FUN_0001466C`
  → `FUN_000143D0` (обновляет 0x1A4/0x1A0 data-строка).

## 7. ИТОГОВАЯ ТАБЛИЦА: вендор vs наш ArmUrb/Worker

| # | Аспект | Вендор | Наш | Что менять |
|---|---|---|---|---|
| 1 | Курсор StartFrame | один на поток, +1/URB, **реверификация `cursor<=now → now+3` при каждой постройке** (0x4DC6/0x3F1B) | 8 слотов, шаг +8, якорь только на старте | ввести глобальный курсор; в ArmUrb перед `StartFrame = cursor` делать GET_CURRENT_FRAME_NUMBER и `if (cursor <= now) cursor = now+3` |
| 2 | Прайм | насос строит первый URB **только при данных ≥ 1 URB в кольце** (порог 0x4A60); пусто → на проводе тишина | 8 URB праймятся немедленно (нули при пустом кольце) | гейтить прайм/ресабмит порогом наличия данных; пустой слот не сабмитить вообще |
| 3 | Underrun-пакет | `Length = got*frameBytes` (короче), `TransferBufferLength = Σ`, кольцо осушено частично | фиксированные 144/288 | копировать фактический недобор и уменьшать Length/TBL; пакет «сколько есть» |
| 4 | Re-arm | **в completion (DPC), нулевой зазор** | completion → событие → воркер PASSIVE | перенести recycle+rearm в completion-рутину (или гарантировать <1 мс реакцию воркера) |
| 5 | Пул | безликий FIFO req (8 шт.), URB = req->0x28, буфер req-персональный | 8 слотов с именами | не критично; но слот-модель при №1 не нужна |
| 6 | Feedback | ISO IN 4Б, TF=3, StartFrame = **fbCursor+32, fbCursor ренчорится `<=now → now+3`**, 2 в полёте, resubmit в DPC | синхронный сабмит 4Б на потоке, StartFrame = «сейчас» (Pace-вычисление) | перевести на 2 асинхронных URB с якорным курсором +32; StartFrame строго в будущем |
| 7 | Hdr.Length | 0xB0 (с запасом) | 0xA4 (точно) | можно оставить |
| 8 | Данные после сабмита | заморожены | заморожены | ✓ одинаково |
| 9 | Стартовые операции | RESET_PIPE + ожидание данных; без EP0 | RESET_PIPE + немедленный прайм | гейт прайма (№2) |
| 10 | Метроном | completion-driven | completion-driven (через поток) | №4 |

Построчный псевдокод стока (финальная форма):

```c
// события: KS-запись данных, завершение URB (DPC), старт/резюм
Pump():                                   // DPC или PASSIVE, спинлок кольца
  while (RingFill() >= FramesPerUrb()) {  // FramesPerUrb = int*2 + carry
      req = PopFree();  if (!req) break;
      now = GetCurrentFrame();
      if (cursor <= now) cursor = now + 3;          // ЯКОРЬ — каждый раз
      urb = req->urb;
      urb->StartFrame = cursor++;
      for (i = 0; i < 2; i++) {
          acc += fracPerPkt;  carry = acc >> 16 /*mod 1000<<16*/;
          want = (carry + intFrames) * 6 + rem;  rem = 0;
          if (want > 298) { rem = want - 298; want = 298; }
          off = i ? Len[i-1] + Off[i-1] : 0;
          got = RingReadInto(req->buf + off, want / 6) * 6;
          Off[i] = off;  Len[i] = got;  rem += got - want;
      }
      urb->TransferBufferLength = Len[0] + Len[1];
      Submit(urb);                                   // IoReuseIrp + IoCallDriver
  }                                                  // пусто → выходим, тишина на проводе

OnOutComplete(req):                        // DPC
  bytesPlayed += req->sum;  NotifyPosition(req->sum);
  PushFree(req);  Pump();                  // без потока, без ожиданий
```

## 8. Не найдено / оговорки

- Перезаполнение данных уже очередного URB — **не найдено** (и не нужно вендору).
- Таймер/периодическая прокачка насоса — **не найдено**: только 3 событийных входа.
- Ёмкость кольца (ring->0x78) до конца не прослежена (задаётся при конфигурации
  фильтра; правдоподобно из латентных таблиц INI) — на расписание не влияет.
- Запись {0x80, X} при ресинке — программный узел управления, USB-трафика
  не порождает (проверено по `FUN_00016338`).

## 9. Внесённые исправления (реализация, итерация 2)

Все расхождения §7 устранены в `Emu/emu_isoch.cpp` + `Emu/emu_wave.cpp`
(движок переписан с «8 слотов + воркер-поток» на вендорскую модель насоса).
Проверено: `kbuild.sh` BUILT, `utest.sh` 6/6 (isoch-юнит починен заодно —
`<ntstrsafe.h>` уехал под `EMU_KERNEL`).

1. **Курсор (§7-1) → глобальный `CursorFrame` + якорь в каждой постройке.**
   `EmuIsochPump` перед каждым `StartFrame` получает текущий кадр шины
   (`EmuIsochBusFrame`) и делает ровно вендорскую проверку 0x4DBE:
   `if ((LONG)(cursor - now) <= 0) cursor = now + 3;` затем
   `StartFrame = cursor; cursor += 1` (трейс-шаг +1/URB). Слоты перестали
   быть «владельцами» кадров: любой затык переносит расписание в будущее.
   - Тонкость DPC: синхронный `EmuUsbSubmitUrb` под спинлоком невозможен.
     Кадровый запрос — выделенный IRP+URB на поток: стек USB3 отвечает
     `GET_CURRENT_FRAME_NUMBER` инлайн (нулевой poll-таймаут легален на
     DISPATCH); если запрос всё же ушёл pending — якорь считается по
     interrupt-time-отображению (`PaceOrigin/PaceBaseFrame/FramesPerMs`),
     калибруемому на старте и обновляемом каждым успешным запросом.
2. **Прайм (§7-2/9) → гейт по данным.** URB без данных не существует:
   волновая сторона отдаёт `RenderAvailable(pos)` = сколько кадров за `pos`
   выпустил OS (`m_LastOsWriteAbs * NotificationFrames`); `available <
   NominalFrames` → насос выходит, провод молчит. Первый URB уходит только
   из `SetWritePacket` (аналог KS-записи вендора: `EmuIsochRenderDataReady`),
   построение при этом ренчорится (п.1). Прайм на старте — через тот же
   насос: пустое кольцо → ноль URB.
3. **Re-arm (§7-4/10) → completion.** Воркер-поток удалён. Completion
   (`DISPATCH_LEVEL`): reap (interlocked `ARMED→FREE`, счётчики, notify) →
   немедленный `EmuIsochPump`. Сабмит — вне спинлока (инлайн-комплишн
   реентерабелен; spinlock не рекурсивен). Останов: retire под
   `PumpLock` → ABORT_PIPE обоих пайпов → дренаж счётчика `Pending`
   (каждый reap −1, ноль ставит `Idle`; 3 с трипвайр) → free. Застрявший
   слот = retention, не free (как раньше).
4. **Feedback (§7-6) → 2 async URB, курсор +32, resubmit в DPC.** Синхронный
   fb-поток удалён. Два слота, перезарядка из своего completion:
   `if ((LONG)(FbCursor - now) <= 0) FbCursor = now + 3; FbCursor += 32;
   StartFrame = FbCursor` — дельта ровно 32 (трейс), строго будущее
   (синхронный «now» больше не попадает в прошлое → xHCI не завершает URB
   мгновенно). Цепочка инлайн-перезарядок ограничена глубиной 4.
5. **Недобор (§7-3) → фактические длины.** `FillRender` возвращает число
   реально упакованных кадров; офсеты пакетов кумулятивные по фактическим
   длинам, `TransferBufferLength` = сумма (вендор 0x4BBD/0x4BBD-форма
   144/138 трейса). Полный недобор (0 кадров) = URB не уходит вовсе (п.2).
6. Прочее: `ShortPackets` считается по фактическим длинам на completion;
   голод насоса считается (`Underruns`, breadcrumb `PumpUnderruns`/`C` при
   останове); статистика IOCTL не менялась (ABI панели заморожен).
