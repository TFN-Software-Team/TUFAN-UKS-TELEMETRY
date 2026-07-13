# DEĞİŞİKLİK NOTU — v1→v2 doküman temizliği

## Doğruluk kaynağı hiyerarşisi

Protokolle ilgili birden fazla doküman var; çelişki çıkarsa aşağıdaki sıra
geçerlidir (üsttekiler alttakileri ezer):

1. **Kod** — `UKS-Telemetry/Core/Src/telemetry.c::Decode_Line()` (UKS
   tarafı) ve `ESP_AKS/lib/Telemetry/Telemetry.cpp::sendStatus()` (AKS
   tarafı). Protokolün fiilen çalışan davranışı budur; herhangi bir
   doküman kodla çelişirse **kod kazanır**, doküman güncellenir.
2. **UKS Uyum Sözleşmesi** — `UKS-Telemetry/README.md` içindeki "UKS
   Uyum Sözleşmesi" bölümü. Kod ile native test (`test/native/test_telemetry_v2.c`,
   55 kontrol, gerçek AKS golden-vektörü dahil) üzerinden birebir
   doğrulanan tek doküman kaynağıdır.
3. **Diğer tüm dokümanlar** — kök `README.md` §4, `AKS/Documents/UKS_LoRa_Protocol.md`,
   vb. Bunlar (2)'den türetilir; aralarında çelişki çıkarsa güncellenmesi
   gereken taraf bunlardır, (2) değil.

## v1 (15-alan) anlatımlarının kaldırılma gerekçesi

Protokol v1'den v2'ye (BMS alanlarının ayrışması: `cellVMax`/`cellVMin`/
`tempH`/`tempL`/`sysState`/`current`/`soc` + `ts_ms`/`spd_x10` eklenmesi,
toplam 19 alan) geçileli bir süre olmasına rağmen iki dokümanda v1 tanımı
hâlâ "güncel" gibi duruyordu:

- `TUFAN-UKS-TELEMETRY/README.md` §4 — 15 alanlı `TEL` formatı, `sequence`
  için imzalı `int32` sınırı (`0..2147483647`), `ver=1`.
- `AKS/Documents/UKS_LoRa_Protocol.md` "AKS -> UKS Telemetry Format" —
  15 alanlı tablo, `ver=1` örnek satır.

Bu, protokolle ilk kez çalışan birinin yanlış alan sayısı/sırasıyla
parser ya da encoder yazmasına yol açabilecek somut bir riskti — nitekim
`UKS-Telemetry/README.md`'deki (artık kaldırılmış) not, ESP_AKS tarafında
BMS alan bölünmesi ile `ts_ms`/`spd_x10` eklenmesinin uzun süre iki ayrı
dalda kaldığını, henüz birleştirilmediğini belirtiyordu. v1 metinleri
**silinmedi, güncel v2 tanımıyla değiştirildi** — tarihsel olarak yanlış
değillerdi, yalnızca artık kodu yansıtmıyorlardı.

`seq`/`ts_ms` için işaretsiz üst sınır (`0..4294967295`) de yakın zamanda
`Parse_U32` ile düzeltildi (bkz. commit `1918430`); v2 tabloları bu son
hâli yansıtıyor.

## Bilinen istisna — ÇÖZÜLDÜ (2026-07-13)

`AKS/Documents/UKS_LoRa_Protocol.md` içindeki "Alan Aralıkları ve AKS
Tarafı Sanitizasyon" bölümünün hemen üstündeki NOT ("yukarıdaki tablo
v1'dir, güncel değildir") yanlıştı — o bölümün üstündeki tablo bu
değişiklikle v2'ye güncellenmişti, yalnızca bu NOT güncellenmeden
bırakılmıştı (o zaman görev kapsamı dışında tutulmuştu). Ayrı bir
görevde ("ESP_AKS'te kodla çelişen bayat yorumları düzelt") bu NOT da
düzeltildi: artık yukarıdaki tablonun da güncel (v2, 19 alan) olduğunu
ve iki tablonun tutarlı olduğunu belirtiyor. Bu iş kalemi kapandı.

## UYUM_NOTU.md

Bu değişiklikler `UYUM_NOTU.md`'ye dokunmadı — bölüm 5 (Sayım
Konvansiyonu) ve bölüm 6 (Teknik Kontrol Checklist) yapısı korunuyor.

## E32/merge-rebase bayat notlarının temizliği (2026-07-09)

`UKS-Telemetry/README.md` ve `Core/Inc/telemetry.h` içinde, ESP_AKS
tarafının 19 alanlı v2 formatını hâlâ iki ayrı dalda (BMS alan bölünmesi +
`ts_ms`/`spd_x10` eklenmesi) tuttuğunu ve bunların merge/rebase ile
birleştirilmesi gerektiğini söyleyen notlar vardı. Bu notlar bayattı:
format artık ESP_AKS `lib/Telemetry/Telemetry.cpp::sendStatus` içinde tek
parçada üretiliyor ve golden fixture'larla doğrulanmış durumda (ESP_AKS
`test/test_native_telemetry/test_telemetry_format.cpp`,
`tools/e2e/test_frame_contract.py`); `AKS/Documents/UKS_LoRa_Protocol.md`
bunu "birebir doğrulandı" olarak belirtiyor.

Aynı notlarda, AKS'in E32 `SPED=0xC4` değerinin yerel UART baud alanını
1200'e düşürebileceğine dair bir uyarı ve `Core/Inc/lora.h` üstündeki
(artık var olmayan) bir `E32_CFG_SPED` yorumuna atıf vardı. Donanım
E32-433T30D'den E22-400T30D-V2'ye geçtiği için bu da geçersizdi; `lora.h`
zaten E22'ye güncellenmiş durumda ve `E32_CFG_SPED` diye bir yorum
içermiyor.

Yapılan değişiklikler:
- `UKS-Telemetry/README.md`: "ÖNEMLİ — AKS Tarafı Durumu (yazıldığı anda)"
  başlığı "Tarihçe (çözüldü)" olarak değiştirildi; içerik güncel duruma
  (format ESP_AKS'te üretiliyor, golden/e2e testlerle doğrulanıyor,
  donanım E22'ye geçti) göre yeniden yazıldı.
- `Core/Inc/telemetry.h`: Başlık yorumundaki aynı bayat "iki ayrı dal"
  notu "NOT (çözüldü)" olarak güncellendi; format tablosu, ölçekler ve
  9.2.a paragrafına dokunulmadı.
- Repo genelinde `E32`/`SPED`/"merge/rebase" grep'lendi; geri kalan tüm
  `E32` referansları (`lora.c`, `main.c`, `main.h`, `e22_regs.h`, kök
  `README.md`) zaten tarihçe/karşılaştırma amaçlı ("E32'den FARKLI",
  "E32 ile AYNI", "E32 → E22 migrasyonu GERÇEKLEŞTİ" gibi) doğru notlar
  olduğu için değiştirilmedi.
- Hiçbir `.c`/`.h` dosyasında kod satırı değişmedi; yalnızca yorum ve
  `.md` satırları düzenlendi.

**Doğrulandı:** `ESP_AKS` deposu `C:\Users\Ravzanur\Desktop\AKS\ESP_AKS`
altında bulunup `TUFAN_UKS_REPO` ortam değişkeni bu repoya (üst dizin)
işaret edecek şekilde ayarlanarak `pytest tools/e2e` çalıştırıldı:
**27 passed, 1 xfailed** (xfail — `test_lb_bms_field_coverage_is_tracked`
— bilinen/takip edilen bir eksiklik, regresyon değil). Özellikle
`test_e32_leftover_constants_are_gone` ve `test_frame_contract.py`
içindeki golden-vektör testleri geçti; bu, README/telemetry.h'de
güncellenen "format ESP_AKS'te tek parçada üretiliyor ve doğrulanmış"
iddiasını teyit ediyor.
