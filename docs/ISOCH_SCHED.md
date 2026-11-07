# emusba10.sys — алгоритм изохронного планирования (полный реверс)

SPDX-License-Identifier: MIT
Метод: дизассемблер (capstone) + декомп Ghidra + **проверка по эталонному трейсу
`_analysis/ref57-stock.pcap`** (USBPcap, оригинальный стек, 65 с потокa 0404).
Все RVA даны для amd64 (imagebase 0x10000; в декомпе имена = VA = RVA+0x10000).

## 0. Результат одной строкой

Оригинал шлёт **1 ISOCH-URB в миллисекунду, в нём 2 iso-пакета по 144 байта
(24 кадра × 6 Б) на микрофреймы 0 и 4 каждого 1-мс кадра**, StartFrame явный
(`GET_CURRENT_FRAME_NUMBER + 3`), глубина очереди **8 URB** (пул), resubmit из
completion, размер пакетов плавно адаптируется по feedback EP 0x81 (16.16,
опрос раз в 32 мс, 2 URB в полёте). Подтверждено трейсом: 65255 URB,
numPackets=2, payload 288 Б (отклонения 282 Б = 144+138), StartFrame +1/URB,
отставание completion ≈ 8 URB, feedback — дельта StartFrame ровно 32.

## 1. Объекты и пулы

**Поток (stream) — 2 вида, создаются в `FUN_0002898C` (RVA 0x1898C) при
SELECT_INTERFACE**: после `FUN_00018F80` (0x18F80, URB SELECT_INTERFACE) и
`FUN_00019180` (0x19180, получить USBD_INTERFACE_INFORMATION) каждый пайп
классифицируется: `MPS < 9` → **sync-поток** (feedback EP 0x81, MPS=4), иначе →
**data-поток** (аудио EP 0x01). Конструкторы: `FUN_000284C0` (0x184C0, sync) /
`FUN_000285BC` (0x185BC, data).

| Поле stream-ctx | Смысл (из дизасма конструкторов) |
|---|---|
| +0x60 | USBDEV-объект |
| +0x68 | **isHighSpeed** (`FUN_00019C40` = vtbl-вызов девайса; 1 → feedback 16.16/4Б, 0 → 10.14/3Б) |
| +0x6C/+0x6D | ifNumber / altSetting |
| +0x70..0x77 | копия `USBD_PIPE_INFORMATION[0..7]`: MPS@+0, EPAddr@+2, **Interval@+3**, PipeType@+4 |
| +0x78 | PipeHandle |
| +0x88 (data) | bmAttributes>>2&3 — sync-тип EP (0 = async) |
| +0x89 (data) | байт 3 CS-endpoint (0x25) дескриптора |
| +0x8C | **шаг StartFrame**: sync: EP-дескриптор bLength<9 → **32**; data: для VID_041E PID {3F02,3F04,3F0A,3F0B} → **5**, иначе 1 |
| +0x90 (data) | флаг «есть feedback-поток»: E-MU и `0xA8 ∈ {2,4}` → **1** |
| +0x90 (sync) | указатель на парный data-поток (линкуется `FUN_00013A6C`, RVA 0x13A6C) |
| +0xA8 | **пакетов на 1-мс кадр**: FS: `Interval`; HS: `8 / (1 << (Interval-1))` |
| +0x94/+0x9C/+0x98 (data) | курсор кадров; смещение старта (FS→6 / HS→3 при явном старте); режим |
| +0x19C | байт на кадр = `(bits>>3) × channels` (`FUN_0001468C`, RVA 0x1468C) |
| +0x198 | частота (Гц) |
| +0x1A4 | **целая часть кадров на пакет** = `(rate/1000) / 0xA8` |
| +0x1A0 | **дробная часть × (1000<<16)** = `(rate%1000)/0xA8 << 16` |
| +0x1A8 | дробный аккумулятор (mod `0x3E80000` = 1000<<16) |
| +0x1AC | байтовый остаток |
| +0x1B0/+0x1B8 (FS) | резервный буфер кадра при «разрыве» StartFrame |

**Запрос (req, 0x78 байт, pool tag `Pcnw` = 0x774E6350)** — `FUN_00013CCC`
(0x13CCC, sync) / `FUN_00014ED0` (0x14ED0, data-FS) / `FUN_000150A8` (0x150A8, data-HS):

| Поле req | Смысл |
|---|---|
| +0x18 | владелец (stream) |
| +0x20 | PIRP (`IoAllocateIrp`, переиспользуется `IoReuseIrp` в `FUN_00019234`, RVA 0x9234) |
| +0x28 | PURB, размер `N×0xC + 0x98` |
| +0x30 | data-буфер: sync = MPS (4 Б); data-FS = MPS (298 Б); data-HS = `N×MPS` |
| +0x38/+0x40 | TransferBufferLength (capture/render-варианты) |
| +0x44 | направление (1 = IN) |
| +0x48 | флаг ASAP (0 → явный StartFrame) |
| +0x4C | NumberOfPackets: **sync = 1** (`(2MPS-1)/MPS`), **data = 0xA8** |
| +0x50 | max packet size (MPS) |
| +0xC | счётчик «предпотреблённых» пакетов (pre-roll) |

**Размеры пулов**: sync — `FUN_00013C4C` (0x13C4C) → **2 запроса**;
data — `FUN_00014728` (0x14728, arg=8) → **8 запросов**.

## 2. Построение ISOCH-URB (вопрос 1)

Рендер: `FUN_00013E98` (0x13E98) — sync/универсальный; data-поток:
`FUN_00014CD4` (0x14CD4). Дизасм 0x3E98–0x3FF9:

```asm
URB+0x00 (word) = N*0xC + 0x98        ; Length
URB+0x02 (word) = 0x000A              ; URB_FUNCTION_ISOCH_TRANSFER
URB+0x18        = stream+0x78         ; PipeHandle
URB+0x20        = IN ? 3 : 0          ; TransferFlags (3 = IN|USBD_SHORT_TRANSFER_OK)
URB+0x24        = TransferBufferLength
URB+0x28        = TransferBuffer
; явное планирование (req+0x48 == 0):
URB+0x20 &= ~4                        ; снять USBD_START_ISO_TRANSFER_ASAP
frame = GET_CURRENT_FRAME_NUMBER      ; FUN_00019480 через IOCTL_INTERNAL_USB_SUBMIT_URB
if (stream->0x88 <= frame) stream->0x88 = frame + 3     ; якорь «не отставать»
URB+0x80 (StartFrame) = stream->0x8C + stream->0x88     ; = cursor + 32 (sync)
stream->0x88 += stream->0x8C
; data-поток (FUN_00014CD4): StartFrame = stream->0x94; stream->0x94 += 1;
;   HS+IN: devObj+0x4FC = cursor - 6 (штамп для resync)
URB+0x84 = N                          ; NumberOfPackets
; пакеты (OUT): offset кумулятивный, length = min(остаток, MPS):
IsoPacket[i].Offset @ URB+0x8C+i*0xC  ; (свой базис 0x8C, не 0x90)
IsoPacket[i].Length @ URB+0x90+i*0xC
; пакеты (IN, capture): Offset = кумулятивный, Length = 0 (размер вычисляет стек
; по соседним offset'ам), prefill буфера шаблоном "hyhuang@atc.creative.com…"
```

x64-оффсеты URB_ISOCH_TRANSFER (сверены с WDK 26100): StartFrame@0x80,
NumberOfPackets@0x84, IsoPacket[]@0x90 (12 Б/пакет). Драйвер пишет пакеты с
базы 0x8C (его WDK-2007 раскладка без ErrorCount) — на современном стеке это
означало бы сдвиг; **работает**, т.к. стеку хватает StartFrame/NumPackets/buffer.

**Resubmit**: `FUN_00019234` → `IoReuseIrp(irp)` + `IOCTL_INTERNAL_USB_SUBMIT_URB`
(0x220003) вниз стека; URB-буфер используется повторно, но каждый раз
**перестраивается** заново (zero + заполнение) — нового аллоцирования нет.

## 3. Микрофреймовое размещение (вопрос 2)

- Пакеты **не** распределяются по микрофреймам вручную: драйвер даёт только
  StartFrame (в 1-мс кадрах) и список пакетов; HCD сама размещает пакет i в
  очередное обслуживаемое окно. Период обслуживания = `8/0xA8` микрофреймов.
- **0404: 2 пакета на 1-мс кадр ⇒ интервал 4 микрофрейма (500 мкс)** ⇒
  `USBD_PIPE_INFORMATION.Interval = 3` (HS-правило `8/(1<<(I-1))` = 2).
  В трейсе каждый URB покрывает ровно 1 кадр (StartFrame +1) с 2 пакетами.
- Выравнивание StartFrame на 8 микрофреймов **не выполняется** — только
  монотонный кадр + якорь `+3` при отставании.
- SPLIT-специфики (FS-in-HS) в коде нет: скорость устройства влияет только на
  формат feedback (0x68: 16.16/4Б при HS, 10.14/3Б при FS) и на lead 0x9C
  (3 кадра HS / 6 кадров FS при явном старте). Трейс показывает 4-байтовый
  feedback и 2 пакета/кадр ⇒ устройство работает как HS с bInterval=3.

## 4. Синхро-поля Filter->Context (вопрос 3)

`KSFILTER->Context` = общий объект фильтра (он же «ring»): +0x20 COM-объект,
+0x28/+0x30 USBDEV, +0x58 спинлок, +0x64/+0x68 курсоры кольца (кадры,
ёмкость +0x78, stride +0x7C, буфер +0x70... — имена пересекаются, контекст
различается по функциям), +0x98 заполнение.

| Поле | Кто пишет | Кто читает |
|---|---|---|
| **+0x70** | SET Id 0x10002 (драйвер пишет 1) — «явный кадр старта» | `FUN_0003BAEC` (0x3BAEC) при переводе пина в RUN |
| **+0x74** | SET Id 0x10002 (значение); GET Id 0x10002 = `GET_CURRENT_FRAME_NUMBER+3`; RUN: если +0x70≠0 → `FUN_00019480(dev@+0x30)` → `+0x74 = frame+3` | тот же RUN: `FUN_000134BC(streams, ctx->0x70, ctx->0x74)` |
| +0x6C | SET Id 0x10000 | в iso-путях не найдено |
| +0x68 | SET Id 0x10001 (**перезаписывает write-курсор кольца!**) | цикл кольца (уровень заполнения) |

**Гипотеза про латентность emusba10.dat (18125×100 нс) НЕ подтвердилась** для
этих полей: в iso-планировании 0x68/0x6C/0x70/0x74 от INI не зависят. Форматы
`USB\VID…Audio.Latency.%d` читаются в `FUN_000269E4`/`FUN_000376F0`
(через ini-ридеры `FUN_00012518`) и идут в KS-свойство задержки / размер
кольца, а не в StartFrame. Явный старт кадра приходит только из панели
(SET 0x10002) либо вычисляется как `frame+3`.

## 5. Старт потока (вопрос 4)

Цепочка RUN: KS-переход пина → `FUN_0003BAEC` → `FUN_000134BC` (0x134BC) →
`FUN_00014004` (0x14004, sync) + `FUN_00015278` (0x15278, data):

1. `FUN_00015278(dataStream, useExplicit=ctx->0x70, frame=ctx->0x74)`:
   state=2; курсор 0x94 = `useExplicit ? frame : GET_CURRENT_FRAME_NUMBER+3`;
   при явном старте 0x9C = 6 (FS) / 3 (HS).
2. **`FUN_00019B20` (0x19B20) = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL**
   (Length 0x28, Function 0x1E) — сброс/clear-stall пайпа перед первым URB.
   (OUT-стрим: STALL сбрасывается, счётчик ошибок обнуляется.)
3. IN-поток: пре-построение всех 8 req (`FUN_00014CD4`, StartFrame=cursor++)
   и **пакетный submit всех сразу** (completion 0x162C8). OUT-поток: req-ов в
   standby нет → сразу `FUN_000148A8` — насос.
4. `FUN_00014004(syncStream, …)`: курсор 0x88 = frame+3; оба req буфера
   заполняются номиналом частоты в feedback-формате
   (FS: `(r/1000<<14)|((r%1000)*0x4000/1000)`, 3 Б; HS: 16.16, 4 Б);
   RESET_PIPE; submit обоих (completion 0x1466C).
5. Первый data-URB уходит с StartFrame = **кадр старта + 3** (3 мс форы), но
   только когда в кольце есть ≥ 1 URB данных; до этого насос молчит.
   Между RUN и первым URB — только RESET_PIPE (нет SET_INTERFACE/EP0/XU:
   в трейсе за 65 с всего 14 control-рекордов).

## 6. Feedback EP 0x81 (вопрос 5)

- Опрос: `FUN_00013A7C` (0x13A7C) — ISOCH IN, **1 пакет × 4 Б (HS)**,
  TransferFlags=3 (IN|SHORT_TRANSFER_OK), StartFrame = cursor+**32**,
  2 req в полёте, resubmit из completion `FUN_000143D0` (0x143D0) —
  перманентный цикл. Трейс: дельты StartFrame ровно 32, ~31 опрос/с.
- Декод (`FUN_000143D0`): HS: `int = value>>16`, `frac_milli = (u16)value × 1000`;
  FS: 3 Б: `int = v>>14`, `frac_milli = (v & 0x3FFF) × 4000`.
- Обновление часов data-потока (через sync->0x90 → dataStream):
  `if (int==cur.int && frac==cur.frac) || (frameSize*int <= MPS)` →
  `data->0x1A4 = int; data->0x1A0 = frac_milli`.
  Для 0404: 0x00180000 → int=24, frac=0 (номинал 24 кадра/пакет — совпадает).
  Отклонения (0x0017F000 → 23.94) дают редкие пакеты 138 Б (трейс: 112 шт.).
- Влияние на размеры (насос `FUN_000148A8`, 0x14A8-цикл пакетов):
  ```
  acc = 0x1A8 + 0x1A0;  0x1A8 = acc % 0x3E80000;              // дробная часть
  bytes = (acc / 0x3E80000 + 0x1A4) * frameSize + 0x1AC;      // целевая длина пакета
  got = ring_read(ring, pktBuf, bytes/frameSize);             // фактические кадры
  IsoPacket[i].Length = got * frameSize;                      // недобор → короче
  0x1AC += IsoPacket[i].Length - bytes;                       // перенос остатка
  ```
  Offset пакета i = `Length[i-1] + Offset[i-1]`; `URB+0x24 = Σ Length`.

## 7. Насос рендера и глубина очереди (вопрос 6)

`FUN_000148A8` (вызывается: после старта, из completion OUT `FUN_00015C88`,
из KS-записи `FUN_00013424`):

```
while (ring_fill >= int*0xA8 + frac_carry):        // данных ≥ 1 URB
    req = pop(free_list)                            // пул 8
    FUN_00014CD4: StartFrame = cursor; cursor += 1  // 1 URB = 1 мс-кадр
    for i in 0..0xA8-1:                             // 2 пакета
        bytes = адаптивная длина (см. §6)
        IsoPacket[i].Offset = cum; IsoPacket[i].Length = ring_read(...)
    URB.TransferBufferLength = Σ Length
    if state == 2: queue(pending); in_flight++; SUBMIT(completion 0x162C8)
    else: queue(standby)                            // пауза
```

Completion OUT (`FUN_00015C88`): при ошибках `0xC0000120/0xC000009D/0xC000000E`
или state==0 — req в free list (очередь схлопывается, событие при счётчике 0);
иначе — recycle + немедленно `FUN_000148A8` → **глубина очереди держится на
размере пула = 8 URB** (трейс: completion отстаёт от submit на ~8 кадров).
Контроль «разрыва» кадров в completion: `urb->StartFrame` против
`stream->0x1BC` (последний виденный): перескок >1 → данные кадра в
аварийный буфер (FS) или прямая запись с пропуском; StartFrame < прошлого →
счётчик 2 («уехавший назад»).

Capture: отдельные req, IN, Length пакетов = 0 (стек считает по offset'ам),
prefill «тихим» шаблоном; completion пишет данные в кольцо
(`FUN_00012F20`) и самоперезапускается. Watcher interrupt-EP — это
`FUN_0002A7B0` (BULK_OR_INTERRUPT IN, 16 URB, completion `FUN_0001888C`) —
к статус-пайпу XU, к планированию отношения не имеет.

## 8. Псевдокод планировщика стока (для повторения)

```c
// Константы для 0404 USB, 48 кГц, 24-бит, 2 канала:
N_PACKETS      = 2;                 // = 8 / (1 << (bInterval-1)), bInterval=3
FRAME_BYTES    = 6;                 // 2ch * 3B
PKT_FRAMES_INT = 48 / N_PACKETS;    // = 24  (rate/1000/N_PACKETS)
PKT_FRAC_MILLI = 0;                 // (rate%1000)/N_PACKETS << 16
MPS            = 298;
QUEUE_DEPTH    = 8;                 // URB в полёте
START_LEAD     = 3;                 // кадров: первый StartFrame = now + 3
FB_ADVANCE     = 32;                // кадров между опросами feedback
FB_DEPTH       = 2;                 // feedback URB в полёте
FB_FORMAT      = 16.16 (4 байта, HS);  // FS был бы 10.14, 3 байта

// Старт:
SendControlUrb(SYNC_RESET_PIPE_AND_CLEAR_STALL, dataPipe);   // Function 0x1E
startFrame = GetCurrentFrame() + START_LEAD;
fractAcc = byteRem = 0;
lastSeenStartFrame = 0;

// Насос (дергается: старт, завершение OUT-URB, поступление данных):
while (RingFill() >= PKT_FRAMES_INT * N_PACKETS) {
    urb = GetFromPool(8);                              // NumberOfPackets = N_PACKETS
    urb->Function = ISOCH_TRANSFER; urb->TransferFlags = 0 (OUT);
    urb->StartFrame = startFrame++;
    cum = 0;
    for (i = 0; i < N_PACKETS; i++) {
        acc  = fractAcc + PKT_FRAC_MILLI;  fractAcc = acc % (1000 << 16);
        want = (acc / (1000 << 16) + PKT_FRAMES_INT) * FRAME_BYTES + byteRem;
        got  = RingRead(urb->buf + cum, want / FRAME_BYTES) * FRAME_BYTES;
        urb->IsoPacket[i].Offset = cum;  urb->IsoPacket[i].Length = got;
        byteRem += got - want;  cum += got;
    }
    urb->TransferBufferLength = cum;
    IoReuseIrp(...); SubmitUrb(urb);                    // в полёте до 8
}

// Завершение OUT-URB (в DPC completion):
if (status == CANCELED/NO_SUCH_DEVICE/…  || stopped) ReturnToPool();
else { Pump(); }                                    // держит 8 в полёте

// Feedback (2 URB, самоперезапуск из completion):
fbUrb: ISOCH, IN|SHORT_TRANSFER_OK, 1 пакет 4 Б;
fbUrb->StartFrame = fbCursor;  fbCursor += 32;       // якорь: if (fbCursor <= now) fbCursor = now+3;
on complete: v = *(u32*)fbBuf;
    frames_int  = v >> 16;                           // 16.16 (HS)
    frames_frac = (u16)v * 1000;                     // милли × 2^16
    if (frames_int == cur_int && frames_frac == cur_frac)
       || (FRAME_BYTES * frames_int <= MPS)
        { cur_int = frames_int; cur_frac = frames_frac; }   // → размеры пакетов
    resubmit (StartFrame += 32);
```

## 9. Проверочные числа из ref57-stock.pcap

| Метрика | Значение |
|---|---|
| URB (EP 0x01, OUT, func 0x0A) | 65255 шт. |
| NumberOfPackets | **2** (всегда) |
| dataLength | 288 (65143), 282 (112) — адаптация feedback |
| StartFrame | монотонно +1 (137235 → 202489) |
| глубина | completion отстаёт от submit на ≈8 |
| Feedback (EP 0x81) | 2039 опросов, StartFrame +32, payload 4 Б = `00 00 18 00` (0x00180000), редкие `00 F0 17 00` |
| Control за 65 с | 14 записей (конфигурация до стрима) |
