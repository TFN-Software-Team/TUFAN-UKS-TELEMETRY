# UKS — Uzaktan Kontrol Sistemi (Yer İstasyonu Firmware'i)

> ⚠️ Yönetmelik 9.2 uyumu ve UKS/AKS kanal ayrımı için bkz. [UYUM_NOTU.md](./UYUM_NOTU.md).

**Repo:** `TFN-Software-Team/TUFAN-UKS-TELEMETRY` · **Branch:** `ravza`
**MCU:** STM32F103 (Cortex-M3) · **HAL:** STM32 HAL (RTOS yok, bare-metal ana döngü)
**Görev:** TUFAN elektrikli araç projesinde, aracın (AKS) gönderdiği telemetriyi alıp ekrana basan yer istasyonu. Yönetmelik 9.2.a gereği RF hattı **tek yönlüdür**: UKS → AKS yönünde yalnızca stabilizasyon teyidi amaçlı 0xB0 heartbeat'i gönderilir, komut kanalı yoktur (bkz. [UYUM_NOTU.md](./UYUM_NOTU.md)).

---

## 1. Genel Mimari

UKS, FreeRTOS **kullanmaz** — tek çekirdekli, kesme-tabanlı bare-metal bir tasarımdır. Sistem üç katmanda çalışır:

1. **ISR katmanı** — yalnızca ham veri toplar, hiçbir parse/format işi yapmaz.
2. **Ana döngü (main context)** — tüm ağır iş (satır birleştirme, parse, dashboard basımı) burada yapılır.
3. **Donanım sürücüleri** (`lora.c`, HAL UART/GPIO) — E22 LoRa modülünü ve UART'ları yönetir.

Bu ayrımın nedeni: HSI 8 MHz'de (klon-güvenli saat, aşağıya bakın) `Decode_Line` en kötü senaryoda ~1 ms sürebiliyor; bu süre 9600 baud'daki byte penceresiyle (~1.04 ms) çakışıp Overrun (ORE) riski doğuruyordu. Çözüm: ISR sadece ring buffer'a yazar (mikrosaniye seviyesinde, sabit süreli), ağır iş ana döngüye taşındı.

### Dosya yapısı

```
UKS-Telemetry/
├── Core/
│   ├── Inc/
│   │   ├── main.h          — pin haritası, sistem durumu
│   │   ├── telemetry.h     — protokol sabitleri, TelCtx_t, public API
│   │   ├── lora.h          — LoraCtx_t, public API (E22 register sabitleri e22_regs.h'de)
│   │   └── e22_regs.h      — E22 register adresleri/hedef değerleri (tek doğruluk kaynağı)
│   └── Src/
│       ├── main.c          — sistem init, ana döngü, heartbeat TX
│       ├── telemetry.c     — CSV parser, frame kuyruğu, dashboard
│       ├── lora.c          — E22 GPIO/config/TX-RX sürücüsü
│       ├── stm32f1xx_it.c  — kesme handler'ları (USART2)
│       └── stm32f1xx_hal_msp.c — HAL MSP init (NVIC öncelikleri burada set edilir)
└── UKS-Telemetry.ioc        — CubeMX proje dosyası (RCC, GPIO, USART tanımları)
```

---

## 2. Donanım — Pin Haritası

| Pin | İşlev | Açıklama |
|---|---|---|
| PA2 / PA3 | USART2 TX/RX | E22 LoRa modülü — **9600 baud** |
| PA9 / PA10 | USART1 TX/RX | Seri monitör / ekran çıkışı — **115200 baud** |
| PB6 | `E22_M0_Pin` | E22 mod seçim biti — Normal=LOW, Config=LOW |
| PB7 | `E22_M1_Pin` | E22 mod seçim biti — Normal=LOW, Config=HIGH |
| PB10 | `LORA_AUX` | E22 hazır/meşgul sinyali — HIGH=hazır, açık-kolektör + pull-up (input) |

> 9.2.a: eski PA0 ve PB11 pin atamaları (uzaktan durdurma donanımı) kaldırıldı — acil durdurma araç üstündeki fiziksel kontaktörle sağlanır, RF/UKS'ten bağımsızdır.

**Saat kaynağı:** HSI 8 MHz, **PLL kapalı** (kasıtlı — bkz. §6, BUG #6). USART1 @ 115200 baud, HSI 8 MHz'de BRR hata payı ~%0.16 (sınır %2'nin çok altında).

**E22 mod tablosu (M0/M1)** — ayrıntılı açıklama için bkz. §3:

| M0 | M1 | Mod |
|---|---|---|
| LOW | LOW | Normal (transparan) — veri gönder/al |
| LOW | HIGH | Config — `C0`/`C1` register komutları |
| HIGH | LOW | WOR (kullanılmıyor) |
| HIGH | HIGH | E22'de kullanılmıyor |

---

## 3. E22-400T30D-V2 LoRa Modül Konfigürasyonu

> **Durum: E32 → E22 migrasyonu GERÇEKLEŞTİ** (bkz. §11). Donanım E32-433T30D'den
> EBYTE **E22-400T30D-V2** (SX1268, 410.125–493.125 MHz, 30 dBm) modülüne
> geçirildi. Modül pin-uyumlu (M0/M1/RXD/TXD/AUX/VCC/GND — kart değişikliği
> yok) ama **konfigürasyon protokolü tamamen farklı**: register-tabanlı
> `C0`/`C1` komutları, farklı config-modu pin seviyeleri, farklı yanıt
> formatı. Register adresleri/hedef değerleri **tek doğruluk kaynağı**
> olarak `Core/Inc/e22_regs.h`'de tutulur; bu bölümdeki değerler o
> dosyadan türetilmiştir, orada değişmeden burada da değişmemelidir.

Boot sırasında `Lora_Init()` şu sırayı izler: GPIO hazırla → AUX HIGH bekle (boot tamamlansın) → config moduna gir (**M0=0, M1=1**) → **tüm register bloğunu (ADDH..CRYPT_L, 9 byte) `C1` ile oku** ve hex dump'la (bench teyidi + read-before-write için) → mevcut değerler hedeften (CRYPT hariç) farklıysa `C0` ile flash'a kalıcı yaz ve yanıtı doğrula → normal moda dön.

### Mod tablosu (M0/M1) — E32'den FARKLI

| M0 | M1 | Mod |
|---|---|---|
| LOW | LOW | Normal (transparan) — veri gönder/al (E32 ile aynı) |
| **LOW** | **HIGH** | **Config** — `C0`/`C1` register komutları (E32'de bu seviye **Sleep** moduydu, kullanılmıyordu; E22'de config modu **budur**, E32'nin M0=HIGH,M1=HIGH'ı **değil**) |
| HIGH | LOW | WOR (kullanılmıyor) |
| HIGH | HIGH | E22'de kullanılmıyor (E32'de config modu buydu) |

### Register haritası (V2 varsayımı — bench dump ile teyit edilecek)

| Adres | Register | Hedef değer | Anlam |
|---|---|---|---|
| 0x00 | `ADDH` | 0x00 | Adres yüksek bayt |
| 0x01 | `ADDL` | 0x00 | Adres düşük bayt (0x0000 = genel/broadcast) |
| 0x02 | `NETID` | 0x00 | V2'de eklendi (V1 haritasında yok) |
| 0x03 | `REG0` | 0x64 | UART 9600 baud \| 8N1, parity yok \| hava hızı 9.6 kbps |
| 0x04 | `REG1` | 0x00 | Alt-paket 240 B \| RSSI ortam gürültüsü KAPALI \| TX gücü kademe 0 (**en yüksek**, T30D: 30 dBm) |
| 0x05 | `REG2` | 0x17 | Kanal 23 → 433.125 MHz |
| 0x06 | `REG3` | 0x00 | **RSSI byte KAPALI** \| transparan mod \| röle kapalı \| LBT kapalı |
| 0x07 | `CRYPT_H` | 0x00 | Yazılır, **geri okunamaz** |
| 0x08 | `CRYPT_L` | 0x00 | Yazılır, **geri okunamaz** |

> **DOĞRULAMA NOTU (bağlayıcı):** E22'nin V1 ve V2 firmware'lerinde register haritası farklıdır (V2'de `NETID` eklendi, sonraki adresler kaydı). Yukarıdaki harita **V2 varsayımıdır**, henüz bench'te doğrulanmadı. `Lora_Init()` her boot'ta bloğu okuyup hex dump basar; bu dump datasheet/gerçek modül davranışıyla karşılaştırılıp adres sabitleri gerekirse **tek yerden** (`e22_regs.h`) düzeltilecek.

**Frekans formülü:** `frekans (MHz) = 410.125 + REG2`. `REG2=0x17` (23 ondalık) → `410.125 + 23 = 433.125 MHz` (433 ISM bandı 433.05–434.79 içinde).

> ⚠️ E32'nin formülü `410 + CHAN` idi (tam sayı ofset); E22'de sabit ofset **`410.125`**'tir — bu farkı gözden kaçırmak kanalı ~125 kHz kaydırır.

**Neden RSSI byte'ı kapalı (REG3 bit7=0):** Açılırsa E22 her alınan paketin sonuna 1 byte RSSI ekler; UKS satır-parser'ı (`telemetry.c`, `TEL_FIELD_COUNT=19` sabit alan sayımına dayanır) ve AKS heartbeat RX'i bu ekstra byte'ı veri sanıp parser'ı bozar. Link kalitesi ileride istenirse ayrı, koordineli bir iş (her iki tarafta protokol versiyon bump'ıyla) olarak yapılır.

**CRYPT neden doğrulanmıyor:** `CRYPT_H`/`CRYPT_L` E22'de yazılabilir ama **geri okunamaz** (okuma her zaman anlamsız/farklı veri döner) — bu yüzden hem read-before-write karşılaştırması hem yazma-sonrası doğrulama bu iki adresi kapsam dışı tutar (`E22_REG_VERIFY_LEN=7`, yalnızca `ADDH..REG3`).

> **Kritik kural (E32'den değişmedi):** AKS ve UKS tarafındaki `REG0`/`REG2`/`REG3` değerleri **birebir aynı olmalı** (fiziksel RF parametreleri). Farklı hava hızı veya kanal seçilirse modüller birbirini hiç duyamaz.

### Config protokolü — read-before-write

Config moduna girildikten sonra `ADDH..REG3` (7 byte) `C1 <addr> <len>` ile okunur ve hedefle karşılaştırılır; **tamamı eşleşiyorsa flash'a YAZMA yapılmaz** (log: `"Tüm alanlar (ADDH..REG3) hedefle aynı — YAZMA ATLANDI (flash omru)"`) — flash yazma döngü ömrünü korumak için. Farklıysa `C0 <addr=0x00> <len=9> <vals...>` ile tam blok (CRYPT dahil) yazılır; yanıt başlığı **`C1 <addr> <len>`**'dir (E32'deki "gönderilenin aynen echo'su, başlık `C0`" mantığı **geçersiz**), ardından `vals` yanıtı CRYPT hariç doğrulanır. `FF FF FF` yanıtı ya da beklenmeyen başlık **kesin hata** sayılır (`LORA_ERR`) — E32'deki "echo gelmese/uyuşmasa da `LORA_OK` dönüp devam et" toleransı E22'de **korunmadı**, çünkü `FF FF FF` E22'de net bir ret sinyalidir.

AUX zaman aşımları: Boot=3000ms, Mode=500ms, CFG=2000ms (E32'den devralınan marjlar, değişmedi).

---

## 4. Protokol — AKS ↔ UKS

### AKS → UKS Telemetri (ASCII CSV, v2, CRLF sonlu)

> **Kaynak:** Bu tablo `UKS-Telemetry/README.md` içindeki "UKS Uyum
> Sözleşmesi" bölümünden türetilmiştir — o bölüm kod (`Core/Src/telemetry.c::Decode_Line`)
> ile birebir doğrulanan, protokolün **tek doğruluk kaynağı**dır. Burada
> uyuşmazlık çıkarsa oradaki tanım geçerlidir.

```
TEL,ver,seq,rpm,torque,motorErr,motorValid,motorTimeout,cellVMax,cellVMin,
    tempH,tempL,sysState,packV,current,soc,bmsValid,ts_ms,spd_x10\r\n
```

**Tam olarak 19 alan** (ilk alan literal `TEL` + 18 sayısal alan) — bu sert bir protokol kontratıdır (`TEL_FIELD_COUNT=19`), alan sayısı/sırası değişirse hem AKS encoder hem UKS parser eş zamanlı güncellenmeli (protokol versiyon bump'ı gerektirir). Alan sayısı 19'dan farklıysa veya `ver != 2` ise paket tümüyle reddedilir.

| # | Alan | Tip | Ölçek | Geçerli aralık | Açıklama |
|---|---|---|---|---|---|
| 0 | `TEL` | — | — | literal | Sabit etiket |
| 1 | `ver` | uint8 | — | 0..255 (`TEL_PROTOCOL_VERSION`=2 zorunlu) | Protokol versiyonu |
| 2 | `seq` | uint32 | — | 0..4294967295 | Sıra numarası (gap/dup tespiti) |
| 3 | `rpm` | uint16 | ham | 0..65535 (sanity ≤20000) | Motor RPM |
| 4 | `torque` | int16 | ham | -32768..32767 | Motor tork geri beslemesi |
| 5 | `motorErr` | uint8 | bit bayrak | 0..255 | Motor hata bayrakları |
| 6 | `motorValid` | uint8 | bool | 0..1 | Motor verisi geçerli mi |
| 7 | `motorTimeout` | uint8 | bool | 0..1 | Motor timeout aktif mi |
| 8 | `cellVMax` | uint16 | ×0.1 mV | 0..65535 | En yüksek hücre voltajı |
| 9 | `cellVMin` | uint16 | ×0.1 mV | 0..65535 | En düşük hücre voltajı |
| 10 | `tempH` | int16 (kaynak int8) | °C | -128..127 | En yüksek BMS sıcaklığı |
| 11 | `tempL` | int16 (kaynak int8) | °C | -128..127 | En düşük BMS sıcaklığı |
| 12 | `sysState` | uint8 | enum | 1..4 | 1=Discharge 2=IDLE 3=Charge 4=FAULT |
| 13 | `packV` | uint16 | ×0.1 V | 0..65535 | Pack voltajı |
| 14 | `current` | int32 | ×0.01 mA | -2147483647..2147483647 | Pack akımı (+şarj / -deşarj) |
| 15 | `soc` | uint16 | ×0.01 % | 0..10000 (10000=%100.00) | Şarj durumu |
| 16 | `bmsValid` | uint8 | bool | 0..1 | BMS verisi geçerli mi |
| 17 | `ts_ms` | uint32 | ms | 0..4294967295 | AKS boot'tan beri geçen süre |
| 18 | `spd_x10` | uint16 | ×0.1 km/h | 0..3000 | Araç hızı |

**Yönetmelik 9.2 şartname eşlemesi** (bkz. [UYUM_NOTU.md](./UYUM_NOTU.md)):

| Alan | Madde | Not |
|---|---|---|
| `tempH` | 9.2.c.ii ("en yüksek batarya sıcaklığı") | Solion BMS bu değeri donanımda hesaplayıp veriyor; AKS yalnızca pass-through yapar |
| `packV` | 9.2.c.iv | — |
| `spd_x10` | 9.2.c.i | — |
| `ts_ms` | 9.2.h | — |

### UKS → AKS yönü: yalnızca heartbeat

Yönetmelik 9.2.a gereği UKS → AKS yönünde eskiden var olan tek-byte komut
kanalı (`UKS_CMD_EMERGENCY_STOP` 0xA1, `UKS_CMD_START` 0xA2, `UKS_CMD_STOP`
0xA3, `UKS_CMD_DRIVE_ENABLE` 0xA4) sistemden tamamen kaldırılmıştır. Bu
yönde gönderilen **tek byte** `LORA_HEARTBEAT_BYTE` (0xB0) — içerik
taşımayan, ~1 Hz periyodik bir stabilizasyon teyididir (bkz. §6). Acil
durdurma araç üstündeki fiziksel kontaktörle sağlanır, RF'ten bağımsızdır.

---

## 5. Telemetri Modülü (`telemetry.c` / `telemetry.h`)

### Mimari — SPSC (tek-üretici/tek-tüketici) deseni iki kademede

**Kademe 1 — RX ring buffer (ISR yazar, ana döngü okur)**
```
Telemetry_RxBytePush()  [ISR, USART2 RxCplt]
  → SADECE rx_ring[rx_head]'e yazar, head'i ilerletir.
  → Parse/tokenize YAPMAZ — sabit süreli, mikrosaniye seviyesinde.
  → Ring doluysa byte düşürülür (ring_overflow sayılır).
```
256 byte boyutunda (`TEL_RX_RING_SIZE`), 9600 baud'da ~266 ms veri tutar — ana döngü dashboard basarken (en kötü ~60ms) bile rahatça yetişir.

**Kademe 2 — Frame kuyruğu (Process üretici, Parse tüketici)**
```
Telemetry_Process()  [ana döngü, her turda çağrılır]
  → Ring'deki bekleyen byte'ları sırayla Process_Byte()'a yollar.
  → Process_Byte satırı line_buf'ta biriktirir, '\n' görünce Decode_Line() çağırır.
  → Decode_Line: Tokenize (19 alan) → her alanı Parse_Int/Parse_U32 ile range-check →
    Track_Sequence (gap/dup tespiti) → Commit_Frame (frame_q'ya yayınla).

Telemetry_Parse()  [ana döngü, dashboard basmadan önce]
  → frame_q'dan en eski frame'i çıkarır (FIFO).
```
`frame_q` derinliği 4 (`TEL_FRAME_Q_DEPTH`) — AKS tarafı link flapping düzeltmesiyle 5 Hz'den 2 Hz'e indi (`LORA_TX_PERIOD_MS=500`), nominal frame aralığı ~500ms iken en kötü bloklama (dashboard basımı ~60ms) pratikte 1 frame bile biriktirmiyor, 4 derinlik rahat marj.

Hem `rx_ring` hem `frame_q` **tek-üretici/tek-tüketici** olduğu için kritik bölüm (PRIMASK/interrupt disable) **gerekmez** — yalnızca `volatile` head/tail yeterli.

### Parse güvenliği

`Parse_Int`, işaretli ondalık tamsayı parser'ı. Son hane kontrolü dahil taşma-güvenli: `v == 214748364 && digit > 7` durumunda erken reddeder — aksi halde `v*10+digit` signed long overflow'a (UB) yol açabilirdi.

`Decode_Line` her alanı hem format hem sanity aralığına göre doğrular (örn. RPM ≤ 20000, SOC ≤ 100, sıcaklık -40..120°C). Tag (`TEL`) ve protokol versiyonu da ayrıca kontrol edilir.

### İstatistikler ve dashboard

`Telemetry_GetStats` üzerinden: rx_bytes, rx_lines, parse_fail, bad_tag, bad_version, range_fail, timeout_drop, overflow_drop, ring_overflow, good_packets, seq_gaps, seq_dup_or_stale. Her 3 saniyede bir heartbeat olarak basılır; `Telemetry_PrintDashboard` ise tam ASCII-art dashboard'u SOC bar'ı, motor/BMS detaylarıyla basar (her 3 frame'de bir — `DASH_EVERY_N`, RX'e nefes alanı bırakmak için throttle'lı).

---

## 6. LoRa Sürücüsü (`lora.c` / `lora.h`)

### TX-safe çekirdek — half-duplex çakışma koruması

USART2'de `HAL_UART_Receive_IT` ile byte-byte RX sürerken doğrudan `HAL_UART_Transmit` çağrılırsa HAL'in `__HAL_LOCK` mekanizması yüzünden TX `HAL_BUSY` dönebilir ya da devam eden RX bozulabilir. Bu, heartbeat gönderiminde telemetri akışının donmasına yol açıyordu.

Çözüm — `Lora_TxSafe`:
1. Devam eden IT-RX varsa güvenli abort et (`rx_active` bayrağıyla takip).
2. Bloklayan TX yap.
3. RX önceden aktifse yeniden arm et.

Tek varyant kaldı:
- **`Lora_Send`** (`block_aux=1`) — AUX HIGH'ı 200ms bekler, meşgulse `LORA_ERR_BUSY` döner. RF hattındaki tek TX kaynağı olan 0xB0 heartbeat'i bununla gönderilir (best-effort — başarısızsa sessizce atlanır, bir sonraki periyotta tekrar denenir).

> 9.2.a: eski `Lora_SendCritical` (`block_aux=0`, komut gönderimi için kullanılırdı) sistemden tamamen kaldırıldı.

---

## 7. Kesme Önceliği Hiyerarşisi

```
SysTick   = öncelik 0  (en yüksek — her zaman ilerlemeli)
USART2    = öncelik 2  (LoRa RX)
```

**Gerekçe:** `HAL_UART_Transmit/Receive` timeout ölçümü için `HAL_GetTick()` kullanır; bu sayacı SysTick kesmesi artırır. SysTick en yüksek öncelikte tutulduğu sürece diğer kesmeler tick ilerlemesini bloklayamaz.

NVIC öncelik ayarının **tek kaynağı** `stm32f1xx_hal_msp.c`'deki `HAL_UART_MspInit`'tir — çünkü bu fonksiyon `MX_GPIO_Init`'ten **sonra** (UART init sırasında) çalışır; `MX_GPIO_Init` içinde set edilmiş olsaydı MspInit tarafından ezilirdi.

---

## 8. Sistem Başlatma Sırası (`main()`)

```
HAL_Init() → SystemClock_Config() (HSI 8MHz, PLL kapalı)
  → MX_GPIO_Init()      — NVIC öncelikleri (SysTick)
  → MX_USART1_UART_Init() (115200 baud — ekran/monitör)
  → MX_USART2_UART_Init() (9600 baud  — LoRa)
  → Telemetry_Init(&tel_ctx)
  → Lora_Init(&lora_ctx, &huart2)   — E22 GPIO/config/register okuma-yazma doğrulama
  → Lora_SetRxByteHandler(...)      — on_lora_rx_byte köprüsü kurulur
  → Lora_StartReceive(&lora_ctx)    — IT-RX başlatılır

Ana döngü (sonsuz):
  now = HAL_GetTick()
  [LORA_HEARTBEAT_PERIOD_MS'te bir] 0xB0 heartbeat TX (Lora_Send)
  [3 saniyede bir] heartbeat istatistik basımı
  Telemetry_Process(&tel_ctx, now)  — ring buffer'ı işle (Tick'ten ÖNCE — bu turki byte'lar işlensin)
  Telemetry_Tick(&tel_ctx, now)     — yarım satır timeout kontrolü
  [frame hazırsa] Telemetry_Parse + throttle'lı dashboard basımı
```

> ⚠️ PB6/PB7 (`E22_M0_Pin`/`E22_M1_Pin`) bilerek `MX_GPIO_Init()` içinde **değil**, `Lora_Init()` → `E22_GPIO_Init()` içinde ayarlanır. Bu sıralama kasıtlı: pinler henüz output değilken LoRa UART init'ten önce modülü yanlışlıkla config moduna almamak için.

---

## 9. Geçmiş Bug Düzeltmeleri (kod içi referans)

main.c başlığında numaralandırılmış, halen geçerli düzeltmeler:

| # | Sorun | Çözüm |
|---|---|---|
| BUG #4 | printf çıktısı hiçbir yere gitmiyordu | `__io_putchar` → USART1 yönlendirmesi; `_write` syscalls.c'den kaldırıldı (çift tanım çakışması önlendi) |
| BUG #6 | Klonlanmış F103'lerde PLL saat hatası | HSI 8 MHz, PLL kapalı (klon-güvenli) |
| BUG #7 | USART1 9600 baud'da dashboard basımı 730ms sürüyor, 200ms timeout aşılıyor | 115200 baud'a çıkarıldı (~60ms'e indi) |
| BUG #9 | `Parse_Int`'te son hane kontrolü eksik, signed long overflow UB riski | Son hane için ayrı taşma kontrolü eklendi |
| FIX-E22 | M0/M1 pinleri floating kalıyor, modül mod belirsizliği → `rx_byte=0` | `Lora_Init()` içinde GPIO config + register okuma/yazma (read-before-write) |
| FIX-B | `_write()` çift tanım riski | syscalls.c'den kaldırıldı, yalnızca `__io_putchar` |
| FIX-C | Her frame'de dashboard basımı RX'i bloklayıp veri kaybına yol açıyordu | `DASH_EVERY_N=3` ile throttle |
| v3 (ISR yükü) | Decode_Line ISR içinde ~1ms sürebiliyor, 9600 baud byte penceresiyle (~1.04ms) çakışıp Overrun riski | Parse main context'e taşındı, ISR sadece ring'e yazar |
| v4 (frame kuyruğu) | Çift tampon tasarımında aynı turda 2 satır gelirse ikincisi düşüyordu | SPSC `frame_q[4]` ile kayıp pratikte sıfırlandı |
| 9.2.a | UKS -> AKS komut kanalı ve arac üstü acil durdurma girişinin donanım zinciri yönetmelikle çelişiyordu | Komut kanalı (0xA1-0xA4) ve donanım zinciri (GPIO/EXTI/NVIC/ISR) tamamen kaldırıldı; RF hattı tek yönlü telemetri + 0xB0 heartbeat'tir |

### Daha önce tespit edilen ve düzeltilen regresyon

`f2d96c6` commit'i (self-merge), o ana kadarki en gelişmiş commit (`acb4757`) içindeki şu özellikleri sessizce dışlamıştı: `Lora_TxSafe`, `E32_WaitAuxHigh`, `Lora_SendCritical`, ring buffer mimarisi, `rx_active` watchdog, NVIC öncelik hiyerarşisi. `main.h`'nin güncel hali bu özelliklerin tamamını içerdiği doğrulandı — **regresyon çözüldü**. Bu olay, merge sonrası HEAD'in beklenen özellikleri içerip içermediğinin her zaman doğrulanması gerektiğinin somut bir örneği.

---

## 10. Test Durumu

UKS-Telemetry'de **native unit test altyapısı vardır**: `telemetry.c` STM32 HAL'e bağımlı değildir (yalnızca `stdint`/`stddef`/`stdio`/`string` kullanır — GPIO/UART register erişimi `lora.c`'de izole edilmiştir), bu yüzden hedef MCU'ya ihtiyaç olmadan doğrudan host `gcc` ile derlenip PC üzerinde çalıştırılabilir.

Test dosyası: `test/native/test_telemetry_v2.c` — `Parse_Int`/`Parse_U32` taşma sınırlarını, `Decode_Line` sanity-check aralıklarını, `Track_Sequence` gap/dup tespitini ve gerçek bir AKS golden-vektörünü kapsayan **55 kontrol**.

**Çalıştırma:**

```bash
cd UKS-Telemetry
make test-native
```

(`Makefile`'daki `test-native` hedefi, host `gcc` ile `test/native/test_telemetry_v2.c` + `Core/Src/telemetry.c`'yi derleyip çalıştırır — arm-none-eabi toolchain'e ihtiyaç duymaz.) Elle çalıştırmak için:

```bash
cd UKS-Telemetry/test/native
gcc -std=c99 -Wall -Wextra -I../../Core/Inc \
    test_telemetry_v2.c ../../Core/Src/telemetry.c \
    -o test_telemetry_v2 && ./test_telemetry_v2
```

Beklenen çıktı: `55 checks, 0 failures`.

---

## 11. Bilinen Açık Konular

- **M0/M1 donanım bağlantısı doğrulandı:** Ravza, M0/M1'in STM32'ye (PB6/PB7) bağlı olduğunu fiziksel olarak teyit etti; `main.h` ve kod tabanı bununla tutarlı.
- **AKS tarafı config-on-boot — ARTIK VAR:** Önceki bilinen boşluk ("production AKS firmware'inde config yazımı yoktur") güncelliğini yitirdi. AKS artık boot'ta kendi E22 modülünü konfigüre ediyor (`vTask_LoRa_UKS` boot sırası + `lib/E22Config`, ESP_AKS commit `0caa38b`). Kalan risk: UKS (`e22_regs.h`) ve AKS (`E22Config`) tarafındaki register hedef değerlerinin **birebir aynı** olduğu henüz çapraz doğrulanmadı — bkz. §3 doğrulama notu.
- **E32 → E22 migrasyonu GERÇEKLEŞTİ (iki tarafta da):** Donanım E32-433T30D'den E22-400T30D-V2'ye (SX1268, 30 dBm) geçirildi; register-tabanlı `C0`/`C1` config protokolü, farklı config-modu pin seviyeleri (M0=0,M1=1) ve register haritası `Core/Inc/e22_regs.h`'de tek doğruluk kaynağı olarak tanımlı (bkz. §3). Karşı uçtaki AKS (ESP32) da E22'ye geçti (`lib/E22Config`). Register haritası UKS tarafında henüz **V2 varsayımı** — bench dump ile teyit bekliyor (bkz. §3 doğrulama notu).
- **VCU state alanı planlı, henüz yok:** AKS telemetri paketine VCU durumu eklenmesi planlanıyor — bu, protokol versiyon bump'ı ve UKS parser'ında koordineli güncelleme gerektirecek (19 alan kuralını bozmadan).

---

## 12. Build

CubeMX tabanlı proje (`UKS-Telemetry.ioc`), Makefile ile `arm-none-eabi-gcc` kullanılarak derlenir. STM32 for VSCode extension için `STM32-for-VSCode.config.yaml` mevcut (hedef: `stm32f1x`, cpu: `cortex-m3`, optimizasyon: `Og`).

```bash
make            # build/ dizinine derler
```

Flash için ST-Link veya benzeri bir programlayıcı gerekir (proje dosyalarında otomatik flash script'i bulunmuyor — manuel `st-flash` veya STM32CubeProgrammer kullanılmalı).
