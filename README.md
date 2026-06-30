<<<<<<< Updated upstream
# UKS — Uzaktan Kontrol Sistemi (Yer İstasyonu Firmware'i)
=======
> ⚠️ Yönetmelik 9.2 uyumu ve UKS/AKS kanal ayrımı için bkz. [UYUM_NOTU.md](./UYUM_NOTU.md).

# 🚗⚡ Elektromobil Güvenlik Kontrol Sistemi
>>>>>>> Stashed changes

**Repo:** `TFN-Software-Team/TUFAN-UKS-TELEMETRY` · **Branch:** `ravza`
**MCU:** STM32F103 (Cortex-M3) · **HAL:** STM32 HAL (RTOS yok, bare-metal ana döngü)
**Görev:** TUFAN elektrikli araç projesinde, aracın (AKS) gönderdiği telemetriyi alıp ekrana basan ve operatörden gelen komutları (özellikle E-STOP) LoRa üzerinden araca ileten yer istasyonu.

---

## 1. Genel Mimari

UKS, FreeRTOS **kullanmaz** — tek çekirdekli, kesme-tabanlı bare-metal bir tasarımdır. Sistem üç katmanda çalışır:

1. **ISR katmanı** — yalnızca ham veri toplar, hiçbir parse/format işi yapmaz.
2. **Ana döngü (main context)** — tüm ağır iş (satır birleştirme, parse, dashboard basımı) burada yapılır.
3. **Donanım sürücüleri** (`lora.c`, HAL UART/GPIO) — E32 LoRa modülünü ve UART'ları yönetir.

Bu ayrımın nedeni: HSI 8 MHz'de (klon-güvenli saat, aşağıya bakın) `Decode_Line` en kötü senaryoda ~1 ms sürebiliyor; bu süre 9600 baud'daki byte penceresiyle (~1.04 ms) çakışıp Overrun (ORE) riski doğuruyordu. Çözüm: ISR sadece ring buffer'a yazar (mikrosaniye seviyesinde, sabit süreli), ağır iş ana döngüye taşındı.

### Dosya yapısı

```
UKS-Telemetry/
├── Core/
│   ├── Inc/
│   │   ├── main.h          — pin haritası, sistem durumu
│   │   ├── telemetry.h     — protokol sabitleri, TelCtx_t, public API
│   │   └── lora.h          — E32 config sabitleri, LoraCtx_t, public API
│   └── Src/
│       ├── main.c          — sistem init, ana döngü, E-STOP ISR, E-STOP TX
│       ├── telemetry.c     — CSV parser, frame kuyruğu, dashboard, encoder
│       ├── lora.c          — E32 GPIO/config/TX-RX sürücüsü
│       ├── stm32f1xx_it.c  — kesme handler'ları (EXTI0, USART2)
│       └── stm32f1xx_hal_msp.c — HAL MSP init (NVIC öncelikleri burada set edilir)
└── UKS-Telemetry.ioc        — CubeMX proje dosyası (RCC, GPIO, USART tanımları)
```

---

## 2. Donanım — Pin Haritası

| Pin | İşlev | Açıklama |
|---|---|---|
| PA0 | `AC_L_STOP` | Acil durdurma butonu — EXTI falling edge, internal pull-up |
| PA2 / PA3 | USART2 TX/RX | E32 LoRa modülü — **9600 baud** |
| PA9 / PA10 | USART1 TX/RX | Seri monitör / ekran çıkışı — **115200 baud** |
| PB6 | `E32_M0` | E32 mod seçim biti — Normal=LOW, Config=HIGH |
| PB7 | `E32_M1` | E32 mod seçim biti — Normal=LOW, Config=HIGH |
| PB10 | `LORA_AUX` | E32 hazır/meşgul sinyali — HIGH=hazır, açık-kolektör + pull-up (input) |
| PB11 | `MOTOR_EN` | Durum çıkışı — HIGH=nominal, LOW=E-STOP aktif |

**Saat kaynağı:** HSI 8 MHz, **PLL kapalı** (kasıtlı — bkz. §6, BUG #6). USART1 @ 115200 baud, HSI 8 MHz'de BRR hata payı ~%0.16 (sınır %2'nin çok altında).

**E32 mod tablosu (M0/M1):**

| M0 | M1 | Mod |
|---|---|---|
| LOW | LOW | Normal (transparan) — veri gönder/al |
| HIGH | HIGH | Config — AT komutları |
| HIGH | LOW | WOR (kullanılmıyor) |
| LOW | HIGH | Sleep (kullanılmıyor) |

---

## 3. E32 LoRa Modül Konfigürasyonu

Boot sırasında `Lora_Init()` şu sırayı izler: GPIO hazırla → AUX HIGH bekle (boot tamamlansın) → config moduna gir → `E32_CFG_*` değerlerini flash'a kalıcı yaz + echo doğrula → normal moda dön.

```c
#define E32_CFG_ADDH    0x00U
#define E32_CFG_ADDL    0x00U
#define E32_CFG_SPED    0xC2U   /* 9600 UART | 8N1 | 2.4 kbps hava hızı */
#define E32_CFG_CHAN    0x17U   /* Kanal 23  | 433 MHz (ISM bandı)       */
#define E32_CFG_OPTION  0x47U   /* Transparan | PP | 250ms | FEC | 30 dBm */
```

### Bit alanı doğrulaması

**SPED = 0xC2 = `1100 0010`**
| Bit | Değer | Anlam |
|---|---|---|
| [7:6] | 11 | UART 9600 baud |
| [5:3] | 000 | 8N1, parity yok |
| [2:0] | 010 | 2.4 kbps hava hızı |

> ⚠️ Eski değer `0x18` = UART 1200 baud + Odd Parity + 0.3 kbps idi — bu yüzden config komutu E32'ye hiç ulaşmıyordu (STM 9600'den konuşuyordu, E32 1200'den dinliyordu). **`0xC2` doğru değerdir, `0x1A` (bit[7:6]=00 → 1200 baud) ile karıştırılmamalı** — bu karışıklık AKS_Sim_ESP tarafında da bir kez yanlış bug raporuna yol açtı.

**CHAN = 0x17 = 23 ondalık** → Frekans = 410 + 23 = 433 MHz (433 ISM bandı içinde). Eski değer `0x06` → 416 MHz, ISM bandı dışındaydı.

**OPTION = 0x47 = `0100 0111`**
| Bit | Değer | Anlam |
|---|---|---|
| [7] | 0 | Transparan mod |
| [6] | 1 | Push-pull çıkış |
| [5:3] | 000 | 250 ms wakeup süresi |
| [2] | 1 | FEC açık |
| [1:0] | 11 | 30 dBm TX gücü (E32-433T30D maksimumu) |

Eski değer `0x44` → 20 dBm, gereksiz güç kaybı.

**Adres: 0x0000 (broadcast)** — her iki yöne de paket gider.

> **Kritik kural:** AKS ve UKS tarafındaki SPED/CHAN/OPTION değerleri **birebir aynı olmalı**. Farklı hava hızı veya kanal seçilirse modüller birbirini hiç duyamaz.

### Config yazma protokolü

6 byte'lık kalıcı yazma komutu: `[0xC0, ADDH, ADDL, SPED, CHAN, OPTION]`. E32 flash'a yazdıktan sonra aynı 6 byte'ı echo olarak geri döner; `Lora_Init` bu echo'yu doğrular ama doğrulama başarısız olsa da (echo gelmezse veya uyuşmazsa) modülün çalışabileceği varsayımıyla `LORA_OK` döndürüp devam eder — bu, gerçek donanımda bazı E32 versiyonlarının echo göndermemesine karşı bilinçli bir tolerans.

AUX zaman aşımları: Boot=3000ms, Mode=500ms, CFG=2000ms (E32 datasheet flash yazma süresi ~200ms'e geniş marj).

---

## 4. Protokol — AKS ↔ UKS

### AKS → UKS Telemetri (ASCII CSV, 5 Hz, CRLF sonlu)

```
TEL,<ver>,<seq>,<rpm>,<torq>,<merr>,<mvalid>,<mtout>,<soc>,<bcurr>,<btemp>,<bvolt>,<bcell>,<berr>,<bvalid>\r\n
```

**Tam olarak 15 alan** — bu sert bir protokol kontratıdır, alan sayısı/sırası değişirse hem AKS encoder hem UKS parser eş zamanlı güncellenmeli (protokol versiyon bump'ı gerektirir).

| # | Alan | Tip | Aralık |
|---|---|---|---|
| 0 | `TEL` | literal | — |
| 1 | protokol versiyonu | uint8 | 0–255 (şu an: 1) |
| 2 | sequence | uint32 | 0–2147483647 |
| 3 | motor RPM | uint16 | 0–65535 (sanity: ≤20000) |
| 4 | motor tork | int16 | -32768..32767 |
| 5 | motor hata bayrakları | uint8 | 0–255 |
| 6 | motor data valid | 0/1 | — |
| 7 | motor timeout aktif | 0/1 | — |
| 8 | BMS SOC | uint8 | 0–100 |
| 9 | BMS akım (deci-A) | int16 | -32768..32767 |
| 10 | BMS sıcaklık (°C) | int16 | -40..120 |
| 11 | BMS pack voltajı (deci-V) | uint16 | 0–65535 |
| 12 | BMS ortalama hücre voltajı (mV) | uint16 | 0–65535 |
| 13 | BMS hata bayrakları | uint8 | 0–255 |
| 14 | BMS data valid | 0/1 | — |

### UKS → AKS Komutlar (tek byte, framing/CRC YOK)

| Sembol | Değer | Anlam |
|---|---|---|
| `UKS_CMD_EMERGENCY_STOP` | 0xA1 | Acil durdurma |
| `UKS_CMD_START` | 0xA2 | IDLE → READY isteği |
| `UKS_CMD_STOP` | 0xA3 | Reset / durdurma |
| `UKS_CMD_DRIVE_ENABLE` | 0xA4 | READY → DRIVE isteği |

E-STOP, paket kaybına karşı **3 kez ardışık** (`TEL_ESTOP_BURST_COUNT`) gönderilir — AKS tarafı tek byte beklediği için herhangi bir RX okumasında E-STOP olarak yorumlanır.

> v1 bilinçli tasarım kararı: RF framing veya CRC yok. Bu, gürültülü RF ortamında tek byte'lık komutların (özellikle DRIVE_ENABLE) yanlış yorumlanma riskini taşır; bilinen ve kabul edilmiş bir sınırlamadır.

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
  → Decode_Line: Tokenize (15 alan) → her alanı Parse_Int ile range-check →
    Track_Sequence (gap/dup tespiti) → Commit_Frame (frame_q'ya yayınla).

Telemetry_Parse()  [ana döngü, dashboard basmadan önce]
  → frame_q'dan en eski frame'i çıkarır (FIFO).
```
`frame_q` derinliği 4 (`TEL_FRAME_Q_DEPTH`) — hava hızı ~450ms/frame iken en kötü bloklama (dashboard basımı ~60ms) pratikte 1 frame bile biriktirmiyor, 4 derinlik rahat marj.

Hem `rx_ring` hem `frame_q` **tek-üretici/tek-tüketici** olduğu için kritik bölüm (PRIMASK/interrupt disable) **gerekmez** — yalnızca `volatile` head/tail yeterli.

### Parse güvenliği

`Parse_Int`, işaretli ondalık tamsayı parser'ı. Son hane kontrolü dahil taşma-güvenli: `v == 214748364 && digit > 7` durumunda erken reddeder — aksi halde `v*10+digit` signed long overflow'a (UB) yol açabilirdi.

`Decode_Line` her alanı hem format hem sanity aralığına göre doğrular (örn. RPM ≤ 20000, SOC ≤ 100, sıcaklık -40..120°C). Tag (`TEL`) ve protokol versiyonu da ayrıca kontrol edilir.

### E-STOP latch

`Telemetry_SetEStopActive` idempotent'tir — yalnızca ilk çağrıda callback tetiklenir, ISR-safe'tir. E-STOP yalnızca UKS tarafında lokal olarak latch'lenir; AKS bu komutu UKS'e geri yansıtmaz.

### İstatistikler ve dashboard

`Telemetry_GetStats` üzerinden: rx_bytes, rx_lines, parse_fail, bad_tag, bad_version, range_fail, timeout_drop, overflow_drop, ring_overflow, good_packets, seq_gaps, seq_dup_or_stale, estop_tx_count. Her 3 saniyede bir heartbeat olarak basılır; `Telemetry_PrintDashboard` ise tam ASCII-art dashboard'u SOC bar'ı, motor/BMS detaylarıyla basar (her 3 frame'de bir — `DASH_EVERY_N`, RX'e nefes alanı bırakmak için throttle'lı).

---

## 6. LoRa Sürücüsü (`lora.c` / `lora.h`)

### TX-safe çekirdek — half-duplex çakışma koruması

USART2'de `HAL_UART_Receive_IT` ile byte-byte RX sürerken doğrudan `HAL_UART_Transmit` çağrılırsa HAL'in `__HAL_LOCK` mekanizması yüzünden TX `HAL_BUSY` dönebilir ya da devam eden RX bozulabilir. Bu, komut/E-STOP gönderiminde telemetri akışının donmasına yol açıyordu.

Çözüm — `Lora_TxSafe`:
1. Devam eden IT-RX varsa güvenli abort et (`rx_active` bayrağıyla takip).
2. Bloklayan TX yap.
3. RX önceden aktifse yeniden arm et.

İki varyant:
- **`Lora_Send`** (`block_aux=1`) — AUX HIGH'ı 200ms bekler, meşgulse `LORA_ERR_BUSY` döner (normal komutlar için).
- **`Lora_SendCritical`** (`block_aux=0`) — AUX'u sadece 50ms bekler, timeout olsa bile **best-effort gönderir** (E-STOP için — en kritik komutun AUX-busy yüzünden düşmesini önler).

### E-STOP uçtan uca akış

```
PA0 falling edge
  → EXTI0_IRQHandler → HAL_GPIO_EXTI_Callback
    → 200ms debounce kontrolü
    → MOTOR_EN = LOW (fail-safe çıkış, anında)
    → Telemetry_SetEStopActive() (lokal latch + callback)
    → estop_tx_pending = 1  (flag set, ISR burada TX YAPMAZ)

Ana döngü, her turda:
  → process_estop_tx()
    → pending ise Telemetry_EncodeEStopBurst (3x 0xA1)
    → Lora_SendCritical ile gönder
    → Başarısızsa pending tekrar set edilir, bir sonraki turda tekrar denenir
```

E-STOP ISR'inin **Transmit çağırmaması** kasıtlıdır — bkz. §7 NVIC önceliği deadlock notu.

---

## 7. Kesme Önceliği Hiyerarşisi

```
SysTick   = öncelik 0  (en yüksek — her zaman ilerlemeli)
EXTI0     = öncelik 1  (E-STOP)
USART2    = öncelik 2  (LoRa RX)
```

**Gerekçe:** `HAL_UART_Transmit/Receive` timeout ölçümü için `HAL_GetTick()` kullanır; bu sayacı SysTick kesmesi artırır. Eğer bir UART işlemi, SysTick'ten **yüksek** öncelikli bir kesme (örn. EXTI0) içinde kilitlenirse SysTick araya giremez, tick artmaz, timeout asla dolmaz → deadlock.

Bu kod tabanında E-STOP ISR'i zaten `HAL_UART_Transmit` çağırmıyor (sadece bayrak set ediyor), yani pratikte bu deadlock oluşmaz — ama kuşak+askı önlemi olarak SysTick en yüksekte tutuluyor.

NVIC öncelik ayarının **tek kaynağı** `stm32f1xx_hal_msp.c`'deki `HAL_UART_MspInit`'tir — çünkü bu fonksiyon `MX_GPIO_Init`'ten **sonra** (UART init sırasında) çalışır; `MX_GPIO_Init` içinde set edilmiş olsaydı MspInit tarafından ezilirdi.

---

## 8. Sistem Başlatma Sırası (`main()`)

```
HAL_Init() → SystemClock_Config() (HSI 8MHz, PLL kapalı)
  → MX_GPIO_Init()      — E-STOP EXTI, MOTOR_EN (fail-safe HIGH önce), NVIC öncelikleri
  → MX_USART1_UART_Init() (115200 baud — ekran/monitör)
  → MX_USART2_UART_Init() (9600 baud  — LoRa)
  → Telemetry_Init(&tel_ctx)
  → Lora_Init(&lora_ctx, &huart2)   — E32 GPIO/config/echo doğrulama
  → Lora_SetRxByteHandler(...)      — on_lora_rx_byte köprüsü kurulur
  → Lora_StartReceive(&lora_ctx)    — IT-RX başlatılır

Ana döngü (sonsuz):
  now = HAL_GetTick()
  process_estop_tx()                — bekleyen E-STOP varsa gönder
  [3 saniyede bir] heartbeat basımı
  Telemetry_Process(&tel_ctx, now)  — ring buffer'ı işle (Tick'ten ÖNCE — bu turki byte'lar işlensin)
  Telemetry_Tick(&tel_ctx, now)     — yarım satır timeout kontrolü
  [frame hazırsa] Telemetry_Parse + throttle'lı dashboard basımı
```

> ⚠️ PB6/PB7 (E32_M0/M1) bilerek `MX_GPIO_Init()` içinde **değil**, `Lora_Init()` → `E32_GPIO_Init()` içinde ayarlanır. Bu sıralama kasıtlı: pinler henüz output değilken LoRa UART init'ten önce modülü yanlışlıkla config moduna almamak için.

---

## 9. Geçmiş Bug Düzeltmeleri (kod içi referans)

main.c başlığında numaralandırılmış, halen geçerli düzeltmeler:

| # | Sorun | Çözüm |
|---|---|---|
| BUG #4 | printf çıktısı hiçbir yere gitmiyordu | `__io_putchar` → USART1 yönlendirmesi; `_write` syscalls.c'den kaldırıldı (çift tanım çakışması önlendi) |
| BUG #5 | E-STOP TX timeout çok uzundu | 50 ms'e indirildi |
| BUG #6 | Klonlanmış F103'lerde PLL saat hatası | HSI 8 MHz, PLL kapalı (klon-güvenli) |
| BUG #7 | USART1 9600 baud'da dashboard basımı 730ms sürüyor, 200ms timeout aşılıyor | 115200 baud'a çıkarıldı (~60ms'e indi) |
| BUG #8 | Boot'ta ilk 200ms'de E-STOP butonu yok sayılıyordu | `last_button_press = (uint32_t)(-2000)` wraparound hilesi |
| BUG #9 | `Parse_Int`'te son hane kontrolü eksik, signed long overflow UB riski | Son hane için ayrı taşma kontrolü eklendi |
| FIX-E32 | M0/M1 pinleri floating kalıyor, modül mod belirsizliği → `rx_byte=0` | `Lora_Init()` içinde GPIO config + flash yazımı |
| FIX-A (Kritik) | E-STOP TX, devam eden IT-RX ile çakışıp telemetriyi donduruyordu | `Lora_SendCritical` — IT-RX güvenli durdur/yeniden başlat |
| FIX-B | `_write()` çift tanım riski | syscalls.c'den kaldırıldı, yalnızca `__io_putchar` |
| FIX-C | Her frame'de dashboard basımı RX'i bloklayıp veri kaybına yol açıyordu | `DASH_EVERY_N=3` ile throttle |
| FIX-D (deadlock önleme) | UART timeout + yüksek öncelikli kesme deadlock riski | NVIC hiyerarşisi: SysTick(0) > EXTI0(1) > USART2(2) |
| v3 (ISR yükü) | Decode_Line ISR içinde ~1ms sürebiliyor, 9600 baud byte penceresiyle (~1.04ms) çakışıp Overrun riski | Parse main context'e taşındı, ISR sadece ring'e yazar |
| v4 (frame kuyruğu) | Çift tampon tasarımında aynı turda 2 satır gelirse ikincisi düşüyordu | SPSC `frame_q[4]` ile kayıp pratikte sıfırlandı |

### Daha önce tespit edilen ve düzeltilen regresyon

`f2d96c6` commit'i (self-merge), o ana kadarki en gelişmiş commit (`acb4757`) içindeki şu özellikleri sessizce dışlamıştı: `Lora_TxSafe`, `E32_WaitAuxHigh`, `Lora_SendCritical`, ring buffer mimarisi, `rx_active` watchdog, NVIC öncelik hiyerarşisi. `main.h`'nin güncel hali bu özelliklerin tamamını içerdiği doğrulandı — **regresyon çözüldü**. Bu olay, merge sonrası HEAD'in beklenen özellikleri içerip içermediğinin her zaman doğrulanması gerektiğinin somut bir örneği.

---

## 10. Test Durumu

UKS-Telemetry'de **native unit test altyapısı yoktur** (AKS_Sim_ESP ve production AKS'in aksine — onlarda PlatformIO `env:native` + Unity ile zengin test kapsamı mevcut). Bunun başlıca nedeni STM32 HAL'e doğrudan bağımlılık (GPIO/UART register erişimi) — `telemetry.c` içindeki saf mantık (`Parse_Int`, `Tokenize`, `Decode_Line`, `Track_Sequence`) aslında donanımdan bağımsızdır ve teorik olarak izole edilip native test edilebilir, ancak şu an bu ayrım yapılmamış.

**Olası gelecek iş:** `telemetry.c`'deki saf parse mantığını HAL bağımlılığından ayırıp (AKS tarafındaki `CanParse` modülü gibi) native test edilebilir hale getirmek — özellikle `Parse_Int` taşma sınırları, `Decode_Line` sanity-check aralıkları ve `Track_Sequence` gap/dup tespiti için.

---

## 11. Bilinen Açık Konular

- **M0/M1 donanım bağlantısı doğrulandı:** Ravza, M0/M1'in STM32'ye (PB6/PB7) bağlı olduğunu fiziksel olarak teyit etti; `main.h` ve kod tabanı bununla tutarlı.
- **AKS tarafı asimetrisi:** UKS boot'ta E32'sini kendi kendine konfigüre eder (`Lora_Init`), ancak production AKS firmware'inde E32 için herhangi bir config yazımı **yoktur** — gerçek AKS modülünün o anki konfigürasyonu bilinmiyor ve UKS ile eşleşmemiş olabilir. Bu bilinen bir mimari boşluktur.
- **E32 → E22 migrasyon olasılığı (henüz planlama aşamasında):** E22 serisi register-tabanlı farklı bir config protokolü kullanıyor (`C1` echo, ters TX güç encoding'i); tam model tanımı (E22 vs E220, T22D vs T30D) netleşmeden bu README'deki §3 geçerliliğini korur, migrasyon olursa ayrı bir bölüm gerekecek.
- **VCU state alanı planlı, henüz yok:** AKS telemetri paketine VCU durumu eklenmesi planlanıyor — bu, protokol versiyon bump'ı ve UKS parser'ında koordineli güncelleme gerektirecek (15 alan kuralını bozmadan).

---

## 12. Build

CubeMX tabanlı proje (`UKS-Telemetry.ioc`), Makefile ile `arm-none-eabi-gcc` kullanılarak derlenir. STM32 for VSCode extension için `STM32-for-VSCode.config.yaml` mevcut (hedef: `stm32f1x`, cpu: `cortex-m3`, optimizasyon: `Og`).

```bash
make            # build/ dizinine derler
```

Flash için ST-Link veya benzeri bir programlayıcı gerekir (proje dosyalarında otomatik flash script'i bulunmuyor — manuel `st-flash` veya STM32CubeProgrammer kullanılmalı).
