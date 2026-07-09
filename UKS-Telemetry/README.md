# UKS-Telemetry (STM32) — Protokol Dokümantasyonu

> Aşağıdaki "UKS Uyum Sözleşmesi" bölümü, bu klasördeki gerçek STM32
> firmware'inin (`Core/Inc/telemetry.h`, `Core/Src/telemetry.c`) AKS
> (ESP32) ile konuştuğu ASCII CSV protokolünü belgeler.

## UKS Uyum Sözleşmesi (AKS ↔ UKS Telemetri Protokolü v2)

**Satır formatı** (CRLF sonlu, 19 alan, ilk alan literal `TEL`):

```
TEL,ver,seq,rpm,torque,motorErr,motorValid,motorTimeout,cellVMax,cellVMin,
    tempH,tempL,sysState,packV,current,soc,bmsValid,ts_ms,spd_x10
```

| # | Alan | Tip | Ölçek | Geçerli aralık | Açıklama |
|---|------|-----|-------|-----------------|----------|
| 0 | `TEL` | — | — | literal | Sabit etiket |
| 1 | `ver` | uint8 | — | 0..255 (== `TEL_PROTOCOL_VERSION`=2 zorunlu) | Protokol versiyonu |
| 2 | `seq` | uint32 | — | 0..4294967295 | Sıra numarası (gap/dup tespiti) |
| 3 | `rpm` | uint16 | ham | 0..65535 (sanity ≤ 20000) | Motor RPM |
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

`seq` ve `ts_ms`, tip sınırlarının tamamını (`0..UINT32_MAX`) kabul eden
isaretsiz bir parser (`Parse_U32`) ile ayrıştırılır — AKS bu iki alanı
uint32 olarak üretir ve sarabilir (`ts_ms` ~49.7 günde sarar); parser bu
sözleşmeyle birebir eşleşir.

Alan sayısı 19'dan farklıysa veya `ver != 2` ise paket reddedilir
(`parse_fail` / `bad_version` istatistiği artar); alan sayısı/versiyon
doğru ama bir değer kendi aralığının dışındaysa da `parse_fail` sayılır
(RPM için ayrıca, tip sınırının altında bir `TEL_RPM_MAX` sanity eşiği
`range_fail` olarak ayrı izlenir).

### CSV Forward Satırı (UKS → PC izleme merkezi)

Her geçerli telemetri paketinde, dashboard throttle'ından (`DASH_EVERY_N`)
bağımsız olarak, USART1 üzerinden ek bir makine-okunur satır basılır:

```
CSV,<timestamp_ms>,<speed_kmh_x10>,<tempH>,<packV>,<soc_hundredths>,<seq>\r\n
```

PC tarafı: `T_bat_C = tempH`, `V_bat = packV / 10`, `hız = spd_x10 / 10`;
kalan enerji PC'de `soc_hundredths`'ten türetilir.

### Tarihçe (çözüldü)

Bu protokol UKS tarafında (bu repo) tamdır ve test edilmiştir
(`test/native/test_telemetry_v2.c`). Önceki bir notta, ESP_AKS deposunda
19 alanın tümünün BMS alan bölünmesi (cellVMax/cellVMin/tempH/tempL/
sysState/current/soc) ile ts_ms/spd_x10 eklenmesi iki ayrı dalda kaldığı
ve henüz birleştirilmediği belirtiliyordu — bu artık geçerli değil.
Format ESP_AKS tarafında `lib/Telemetry/Telemetry.cpp::sendStatus`
içinde tek parçada üretiliyor ve golden fixture'larla doğrulanmış
durumda (ESP_AKS `test/test_native_telemetry/test_telemetry_format.cpp`,
`tools/e2e/test_frame_contract.py`); ayrıca `AKS/Documents/
UKS_LoRa_Protocol.md` formatın birebir doğrulandığını belirtiyor.
Donanım da E32'den E22-400T30D-V2'ye geçti; E22 register konfigürasyonu
AKS tarafında `lib/E22Config` + boot dizisiyle uygulanıyor — E32'ye özgü
`SPED` baud endişesi artık geçerli değil.

---

> **Not:** Bu dosyada daha önce "Elektromobil Güvenlik Kontrol Sistemi"
> adlı, bu firmware ile ilgisiz eski bir konsol demosu placeholder olarak
> duruyordu. Arşiv amaçlı [`docs/legacy/elektromobil-guvenlik-demo.md`](../docs/legacy/elektromobil-guvenlik-demo.md)'ye taşındı.
