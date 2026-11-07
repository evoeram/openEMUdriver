# Панельный API `emusba10.sys` — полный реверс (GUID `{F6D7C04D}`)

SPDX-License-Identifier: MIT
Источники: декомпиляция Ghidra `emusba10.sys` (amd64, 1.30.07),
`EmuUsbAudioCPServices.dll`, `emasio.dll`, бинарный разбор PE (pefile/capstone),
WDK 10.0.26100 (сверка структур URB/KS). Все оффсеты — RVA/VA ядра
(imagebase 0x10000; в декомпе Ghidra имена функций = VA).

## 1. Сводка

| Параметр | Значение |
|---|---|
| Транспорт | `DeviceIoControl(handle, …)` по device interface драйвера |
| IOCTL | `0x2F0003` = `IOCTL_KS_PROPERTY` (`CTL_CODE(FILE_DEVICE_KS=0x2F, 0, METHOD_NEITHER, FILE_ANY_ACCESS)`) |
| Сопутствующие | `0x2F0007` = `IOCTL_KS_METHOD`, `0x2F000B` = `IOCTL_KS_ENABLE_EVENT` |
| GUID набора (фильтровый) | `{F6D7C04D-C5C3-4181-9ED7-504ABE0CDDFF}` |
| Формат ввода GET | `KSPROPERTY` (0x18) + payload до 0x30 |
| Формат ввода SET | `KSPROPERTY` (0x18) + payload; данные SET лежат **во входном** буфере |
| Регистрация в ядре | 4 таблицы `KSPROPERTY_SET` (по одной на фильтр), RVAs `0xFAF0`, `0xFB88`, `0x107D0`, `0x11010`; itemsize в файле = 0 (заполняется ядром KS при загрузке), таблицы items по 0x48 байт |
| Хендл | `CreateFileW(<DeviceInterface>, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL)` — так делает панель |

GUID собран из декомпа панели (`local_30..local_24`) и присутствует в `.rdata`
ядра по RVA `0xF6E0` (4 ссылки-указателя). В RE_NOTES ранее был искажён
(«…79EFFDD0CBE») — правильный вариант: `9ED7-504ABE0CDDFF`.

## 2. Два уровня API

Панель работает с драйвером на **двух уровнях**, оба через `IOCTL_KS_PROPERTY`:

**A. Фильтровый (наш предмет):** набор `{F6D7C04D-…}` — доступ к дескрипторам USB,
строкам/нику, прямому EP0, старту/стопу потока, синхро-якорю. Id: `0x00–0x07`,
`0x20`, `0x21` (+ `0x10000–0x10002` на двух фильтрах).

**B. Узловой (топология KS):** наборы `{26BB<код>-<PID>-<VID>-896A-CAAAA123 00 B8}`,
где `<код>` = XU `wExtensionCode` (0xE301…0xE306). Флаги `0x10000001`/`0x10000002`
— это `KSPROPERTY_TYPE_TOPOLOGY|GET/SET`, структура ввода — `KSP_NODE` (0x20):
`{ KSPROPERTY; ULONG NodeId; ULONG Reserved; }`. Привязка node↔набор — через
`KSPROPSETID_Topology` Id 1 (`KSPROPERTY_TOPOLOGY_NODES`), поиск узлов типа
`KSNODETYPE_DEV_SPECIFIC` `{941C7AC0-C559-11D0-8A2B-00A0C9255AC1}`.
Семантика уровней B — это XU-протокол, см. `PROTOCOL.md`; здесь зафиксирован
только KS-мост.

## 3. Структуры (C)

```c
// Фильтровый набор {F6D7C04D}: KSPROPERTY + payload (всё в INPUT-буфере)
typedef struct EMU_PANEL_REQ {          // размер 0x18..0x30
    GUID      Set;                      // {F6D7C04D-C5C3-4181-9ED7-504ABE0CDDFF}
    ULONG     Id;                       // см. таблицу ниже
    ULONG     Flags;                    // 1 = GET (KS GET), 2 = SET
    union {                             // payload @+0x18 (интерпретация зависит от Id)
        struct {                        // Id 7 (GET/SET), мин. размер ввода 0x30
            UCHAR UrbFunction;          // +0x18: код URB_FUNCTION (панель шлёт 0x20)
            UCHAR BRequest;             // +0x19: bRequest SETUP-пакета
            USHORT WValue;              // +0x1A
            USHORT WIndex;              // +0x1C
            ULONG _pad;                 // +0x20 (не читается ядром на GET)
            ULONG DataSize;             // +0x24: только SET — размер данных
            UCHAR Data[0x100];          // +0x28: только SET — данные
        } ep0;
        struct {                        // Id 4 (GET), ввод 0x20
            UCHAR StringIndex;          // +0x18: индекс строки (0 = список LANGID)
            UCHAR _pad;                 // +0x19
            USHORT LangId;              // +0x1A (напр. 0x0409)
        } str;
        struct {                        // Id 2 (GET), ввод ≥ 0x28
            ULONG InterfaceNumber;      // +0x18
            ULONG AlternateSetting;     // +0x1C
            ULONG InterfaceClass;       // +0x20
            ULONG InterfaceSubClass;    // +0x24
            ULONG InterfaceProtocol;    // +0x28
        } iface;
        struct {                        // Id 3/5/6 (GET)
            UCHAR b0;                   // +0x18
            UCHAR b1;                   // +0x19
            UCHAR b2;                   // +0x1A
            UCHAR b3;                   // +0x1B
        } b;
    };
} EMU_PANEL_REQ;

// Ответы (OUTPUT-буфер)
// Id 0 → 18 байт: образ USB device descriptor (bLength=0x12, bDescriptorType=1,
//        bcdUSB@2, bDeviceClass@4, VID@+8, PID@+10, … iManufacturer@14, iProduct@15,
//        iSerialNumber@16, bNumConfigurations@17)
// Id 1 → wTotalLength байт: полный образ configuration descriptor set
//        (при малом буфере — первые 9 байт = заголовок конфигурации)
// Id 2 → 9 байт: interface descriptor (найден по 5 критериям)
// Id 3 → 7 байт: endpoint descriptor (сырой образ)
// Id 4 → USB string descriptor: {bLength, bDescriptorType=3, UTF-16LE …};
//        индекс 0 → список LANGID как UTF-16
// Id 5 → CS-интерфейсный блоб (AC-дескрипторы, wTotalLength байт) или одиночный дескриптор
// Id 6 → CS-endpoint дескриптор (тип 0x25)
// Id 7 → ответ EP0-запроса (размер = RequestedBytes)
// Id 0x20 (GET) → 9-байтный контракт (возвращается статус vtbl-вызова)
```

## 4. Таблица свойств `{F6D7C04D}` (из KSPROPERTY_ITEM ядра)

Идентична на всех 4 фильтрах (кроме отмеченного). `minP` = минимальный
InputBufferLength, `minD` = минимальный OutputBufferLength.

| Id | GET | SET | minP | minD | Семантика |
|----|-----|-----|------|------|-----------|
| 0x00 | ✓ | — | 0x18 | 18 (0x12) | Кешированный **device descriptor** (18 байт) из объекта устройства |
| 0x01 | ✓ | — | 0x18 | 9 | Кешированный **configuration descriptor set** (wTotalLength байт; урезается до 9-байтного заголовка, если буфер мал) |
| 0x02 | ✓ | — | 0x30 | 9 | Поиск **interface descriptor** по (InterfaceNumber, AltSetting, Class, SubClass, Protocol) — `USBD_ParseConfigurationDescriptorEx`; payload = 5 ULONG |
| 0x03 | ✓ | — | 0x20 | 7 | **Endpoint descriptor** (7 байт): payload {bInterfaceNumber@0x18, bAltSetting@0x19, bEndpointAddress@0x1A} |
| 0x04 | ✓ | — | 0x20 | 4 | **Строка устройства** (ник): payload {StringIndex@0x18, LangId@0x1A}. Индекс 0 → LANGID-лист. Источник: INI → затем `GET_DESCRIPTOR(String)` по EP0. Ответ = USB string descriptor ({len, 3, UTF-16…}), max 0xFE |
| 0x05 | ✓ | — | 0x20 | 9 | **Class-specific interface descriptor(s)**: payload {ifnum@0x18, alt@0x19, bDescriptorType=0x24@0x1A}. Для аудио/MIDI-интерфейса (subclass 1/3) возвращает `wTotalLength` байт AC-блоба |
| 0x06 | ✓ | — | 0x20 | 7 | **CS endpoint descriptor** (0x25): payload {ifnum@0x18, alt@0x19, bEndpointAddress@0x1A, bDescriptorType=0x25@0x1B} |
| 0x07 | ✓ | ✓ | 0x30 | 0 | **Прямой EP0-запрос** (URB `CONTROL_TRANSFER_EX`): payload {UrbFunction@0x18, bRequest@0x19, wValue@0x1A, wIndex@0x1C}; GET: буфер = output (размер = OutputBufferLength); SET: {DataSize@0x24, Data@0x28} внутри input |
| 0x20 | ✓(A) | ✓ | 0x18 | 0 | **Старт** потока/устройства: SET→vtbl+0x68(obj,1); GET(A)→vtbl+0x60(obj,1) |
| 0x21 | — | ✓ | 0x18 | 0 | **Стоп**: SET→vtbl+0x68(obj,0) |
| 0x10000 | — | ✓ | 0x18 | 4 | Фильтры C/D: `ctx->0x6C = *(ULONG*)Data` (параметр синхронизации 1) |
| 0x10001 | — | ✓ | 0x18 | 4 | `ctx->0x68 = *(ULONG*)Data` (параметр синхронизации 2) |
| 0x10002 | ✓ | ✓ | 0x18 | 4 | Синхро-якорь: GET → `frameNumber(URB GET_CURRENT_FRAME_NUMBER)+3`; SET → `ctx->0x70=1; ctx->0x74 = *(ULONG*)Data` |

(A) — только на фильтрах с таблицами `0xFAF0`/`0xFB88`.

Коды ошибок: `0xC0000023` (BUFFER_TOO_SMALL — мал output), `0xC000000D`
(INVALID_PARAMETER — мал payload/не найдено), `0xC0000001` (нет данных),
`0xC0000022` (IRQL ≥ DISPATCH на EP0-пути).

## 5. Использование панелью (EmuUsbAudioCPServices.dll, класс CHulaServices)

| Вызов (функция DLL) | Запрос | Назначение |
|---|---|---|
| `FUN_10003ca0` (при открытии) | GET Id 0 → 0x12 байт; затем GET Id 4 (StringIndex=байт из состояния, LangId=0x0409, буфер 0xFE) | Взять VID/PID (для сборки GUID'ов узлов) и ник устройства |
| `FUN_10003ad0` | GET Id 7, ввод 0x30: {UrbFunction=0x20, bRequest=0x2A, wValue=0, wIndex=0}, output 1 байт | Вендорный статус EP0 (0/1/2) |
| `FUN_10003bc0` | GET Id 7, ввод 0x30: {UrbFunction=0x20, bRequest=0x01, wValue=0, wIndex=0}, output 32 байта | Вендорное чтение EP0 (блок состояния, 32 байта — «метры» уровня фильтра) |
| `FUN_100040c0/41e0/42f0/44a0/45b0` | Узловой уровень B | См. §8 |
| `FUN_100043e0`, `FUN_10004ad0` | `0x2F0007` / `0x2F000B` | KS-методы и события |

GUID'ы узлов панель собирает **в рантайме из VID/PID** (полученных из Id 0):
4 набора `{26BBE301|302|303|304-<PID>-<VID>-896A-CAAAA123 00 B8}`.

`emasio.dll` (ASIO) использует тот же IOCTL теми же обёртками
(`FUN_02001df4` = sync-DeviceIoControl) и биндит узлы
`{26BBE301}`, `{26BBE302}`, `{26BBE305}`, `{26BBE3FE}`;
`emaudsv.exe` — те же константы (GUID `{F6D7C04D}` в .rdata @0x818,
`0x2F0003` ×5, `{26BBE301}` @0x2bb9).

## 6. Ядро: карта функций (RVA ← VA декомпа)

| RVA | Роль |
|-----|------|
| `0x30780` | GET-хендлер {F6D7C04D} фильтров A (таблицы 0xFAF0/0xFB88, объект `ctx+0x28`) |
| `0x30BF8` | SET-хендлер фильтров A |
| `0x2E1DC` / `0x2E63C` | GET/SET фильтров C (таблица 0x107D0, объект `ctx+0x30`) |
| `0x27F94` / `0x2842C` | GET/SET фильтра D (таблица 0x11010, объект `ctx+0x30`, + Id 0x10000–0x10002) |
| `0x094DC` | obj: вернуть device descriptor (obj+0x20) |
| `0x094F8` | obj: вернуть config descriptor (obj+0x38) |
| `0x09514` | `USBD_ParseConfigurationDescriptorEx` обёртка |
| `0x0957C` | поиск endpoint descriptor'а (iface+alt+addr) |
| `0x0975C` | поиск class-specific descriptor'а после interface |
| `0x09808` | поиск CS-endpoint после endpoint |
| `0x09930` | **EP0-исполнитель**: собирает URB и зовёт `0x09328` |
| `0x09328` | `IOCTL_INTERNAL_USB_SUBMIT_URB` (0x220003) вниз нижнему драйверу, sync/async по IRQL |
| `0x1BC28` | строка устройства: INI → EP0 string descriptor |
| `0x2B790` | инициализация таблицы LANGID/строк (obj+0x300, счётчик obj+0x4F8) |
| `0x19480` | `URB GET_CURRENT_FRAME_NUMBER` (Length=0x20, Function=7) |
| `0x33274` | выбор топологических таблиц по модели; сборка GUID `{26BB0000|XU - …}` @ctx+0x6C |
| `0x330A8` | построение KSAUTOMATION_TABLE узла: items по 0x48, MinProperty=0x20 (`KSP_NODE`), GET=`0x3AD5C`, SET=`0x3ADF8` |
| `0x3AD5C/0x3ADF8` | узловые GET/SET → `FUN_0001AA2C(node, bReq, CS=Id+1, ch, data, size)` / `FUN_0001AA74` |

Объект «USB-устройство» (на что указывают `ctx+0x28`/`ctx+0x30`):
`+0x20` кеш device descriptor (18 байт; VID@+8, PID@+10, iMfr/iProd/iSerial@+14..16),
`+0x38` кеш configuration descriptor, `+0x300` таблица строк {тип:1=INI,2=EP0; LANGID}
по 4 байта, `+0x4F8` число записей, `+0xF8` буфер строки по умолчанию.
COM-объект девайса — `ctx+0x20` (vtbl: `+0x60` get-state, `+0x68` set-state,
`+0x98` чтение реестра-профиля); поля синхронизации фильтра: `ctx+0x68/6C/70/74`.

## 7. URB/EP0 слой (для Id 7 и строк)

`FUN_00019930(obj, Function, bRequest, wValue, wIndex, buf, size, &transferred, isGet, timeout)`
строит URB `CONTROL_TRANSFER_EX` (x64, **Length=0x88** — сверено с WDK 26100):

```
+0x00 USHORT Length = 0x88        +0x24 ULONG TransferBufferLength
+0x02 USHORT Function             +0x28 PVOID TransferBuffer
+0x04 Status                      +0x30 PMDL  TransferBufferMDL
+0x08 UsbdDeviceHandle            +0x38 UrbLink/Timeout (=0)
+0x20 ULONG TransferFlags         +0x80 UCHAR SetupPacket[8]
      (bit0 = IN)                        {bmReqType@0x80, bRequest@0x81,
                                        wValue@0x82, wIndex@0x84, wLength@0x86}
```

- GET: `TransferFlags=1` (IN), буфер нулится и копируется обратно по факту.
- SET: `TransferFlags=0` (OUT), данные копируются во временный пул (`'Wdm'` tag).
- `UrbFunction` берётся **из payload панели** (0x20; XU-путь ядра использует 0x1B
  `CLASS_INTERFACE`, 0x1C `CLASS_ENDPOINT`, 0x1E `SYNC_RESET_PIPE_AND_CLEAR_STALL`).
- GET-вариант XU-моста шлёт `bRequest|0x80` (0x81/0x82/0x83/0x84) — прошивка
  различает GET/SET по биту 7 bRequest, а не по bmRequestType (согласуется с тем,
  что 0xA1+0x01 сталлится, см. RE_NOTES).
- Квирки оригинала: `Status`, `UsbdDeviceHandle`, `PipeHandle`(+0x18) и `wLength`
  SetupPacket **не инициализируются** (мусор стека); на GET Id 0x10002
  `FrameNumber` читается с +0x18 (раскладка WDK 2007), тогда как современный
  стек пишет его в +0x14 — на Win10 значение может быть мусорным.

## 8. Узловой уровень B: {26BBExx} → XU

```
GUID узла: {26BB0000|XUcode - PID - VID - 896A-CAAAA123 00 B8}
           XUcode ∈ 0xE301..0xE306 (wExtensionCode XU-юнита)
Запрос:    KSP_NODE { Set=GUID; Id; Flags=0x10000001|0x10000002; NodeId; 0 }
Привязка:  GET KSPROPSETID_Topology({720D4AC0-7533-11D0-A5D6-28DB04C10000}) Id=1
           (KSPROPERTY_TOPOLOGY_NODES) → KSMULTIPLE_ITEM список GUID'ов узлов;
           фильтровать по KSNODETYPE_DEV_SPECIFIC {941C7AC0-C559-11D0-8A2B-00A0C9255AC1},
           для каждого кандидата пробовать GET Id=1 на своём наборе — кто ответил,
           тот NodeId и сохраняется.
Семантика: Id узла N ↔ XU CS = N+1 (SET: `FUN_0001AA2C(node, 1, Id+1, ch, buf, size)`;
           GET: `FUN_0001AA74(node)` — чтение текущего значения юнита);
           исключение: Id=1 с size=4 — спец-запрос CS=1 4 байта (частота).
События:   KSEVENTSETID_AudioControlChange {E85E9698-FA2F-11D1-95BD-00C04FB925D3}
           через IOCTL_KS_METHOD (0x2F0007) на узле + IOCTL_KS_ENABLE_EVENT (0x2F000B)
           — панель получает хэндл события и просыпается на изменения юнита
           (пакеты `c0 0e` на interrupt EP 0x83, см. RE_NOTES).
```

Примеры из панели: `{26BBE303}` Id 1 GET 4 байта = цифровая входная частота
(44100…192000, enum eSampleRate 0..5); `{26BBE301}` Id 2 SET 1 байт;
`{26BBE304}` Id 3 SET 1 байт (soft limit); emasio биндит `{26BBE305}` (Direct Monitoring).

## 9. INI/реестр

| Ключ | Смысл |
|---|---|
| `USB\VID_%04X&PID_%04X.%04X` (секция INI, ключ = индекс строки) | Строки/ник устройства (тип 1 в таблице obj+0x300); значения могут быть `\xNN`-escape (UTF-16) |
| `USB\VID_%04X&PID_%04X.LANGID` | Приоритетный LANGID (панель/файл LANGFILE) |
| `USB\VID_%04X&PID_%04X.FIXUP.%03X` (ключи `iManufacturer`, `iProduct`, `iSerialNumber`, числовые) | Патчи дескрипторов после чтения |
| `USB\VID_%04X&PID_%04X[.%03X].Audio.Latency.%d` | Таблицы латентности (см. RE_NOTES) |
| `USB\VID_%04X&PID_%04X[.%03X].Midi.DirectMusic` (ключ `Enable`) | MIDI-фильтр |
| `Configuration`/`CFGFILE`, `Language`/`LANGFILE` | Через COM-объект (vtbl+0x98) |

## 10. Статус верификации

- Таблицы свойств, хендлеры, payload-смещения — статически из бинарника amd64
  (i386-версия не сверялась).
- x64-раскладка URB `CONTROL_TRANSFER_EX` сверена с WDK 10.0.26100 (`shared/usb.h`):
  SetupPacket@0x80, sizeof=0x88 — совпадает с записями драйвера.
- Живой прогон (panel probe против установленного emusba10 + usb-трейс) — не
  выполнялся в этой сессии; Id 7/0x10002 и INI-ник имеют перечисленные в §7 квирки.
- `0x26BBE3FE` в emasio — вспомогательная привязка (вероятно, probe «есть ли
  DevSpecific-узел вообще»); точная роль не восстановлена.
