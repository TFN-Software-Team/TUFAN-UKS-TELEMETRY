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

## Bilinen istisna — kasıtlı olarak dokunulmadı

`AKS/Documents/UKS_LoRa_Protocol.md` içindeki "Alan Aralıkları ve AKS
Tarafı Sanitizasyon" bölümünün hemen üstündeki NOT ("yukarıdaki tablo
v1'dir, güncel değildir") artık **yanlış** — o bölümün üstündeki tablo bu
değişiklikle v2'ye güncellendi. Ancak "Alan Aralıkları ve AKS Tarafı
Sanitizasyon" bölümüne (bu notu da içeren blok) görev kapsamında
dokunulmaması açıkça istendiği için not güncellenmedi. Bu, ayrı bir
onay/iş kalemi olarak ele alınmalı.

## UYUM_NOTU.md

Bu değişiklikler `UYUM_NOTU.md`'ye dokunmadı — bölüm 5 (Sayım
Konvansiyonu) ve bölüm 6 (Teknik Kontrol Checklist) yapısı korunuyor.
