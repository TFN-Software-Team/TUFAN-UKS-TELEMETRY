# UYUM NOTU — UKS (Uzaktan Kontrol Sistemi) ve Yönetmelik 9.2

> Bu doküman, UKS-Telemetry projesinin Yönetmelik madde 9.2 ile ilişkisini
> netleştirmek için hazırlanmıştır. Kod değişikliği içermez; mevcut
> protokol davranışını (bkz. `UKS-Telemetry/Core/Inc/telemetry.h`,
> `UKS-Telemetry/Core/Src/main.c`) referans alır.

## 1. Sistem Ayrımı

UKS üzerinden geçen veri akışı, fiziksel olarak aynı LoRa hattını paylaşsa
da **iki ayrı mantıksal kanal** içerir. Bu ayrımın yapılması, yönetmeliğin
hangi maddesinin hangi kanala uygulandığını netleştirmek için kritiktir.

### (a) Telemetri Kanalı — AKS → UKS → PC, TEK YÖNLÜ

Araçtaki AKS, sensör/durum verisini ASCII CSV `TEL,...` çerçeveleri olarak
UKS'e gönderir; UKS bunu ayrıştırıp PC'ye (izleme merkezi) iletir. Bu kanal
**tek yönlüdür**: veri yalnızca araçtan yer istasyonuna akar.

**Yönetmelik madde 9.2 yalnızca bu kanalı kapsar.** Tek-yönlülük kuralı
(9.2.a) burada uygulanır.

### (b) Uzaktan Kontrol Kanalı — KALDIRILDI (9.2.a)

Komut kanalı ve UKS E-STOP butonu 9.2.a uyumu için sistemden tamamen
kaldırıldı (2026-07-03). RF hattı tek yönlü telemetri + 0xB0 stabilizasyon
teyididir. Acil durdurma araç üstü fiziksel kontaktörledir, RF'ten
bağımsızdır.

## 2. İzin Verilen Geri Bildirim — Heartbeat (0xB0)

Madde 9.2.a, telemetri tek-yönlülüğüne istisna olarak **stabilizasyon
teyidi** amaçlı geri bildirime izin vermektedir. Bu kapsamda, UKS → AKS
yönünde periyodik bir **heartbeat (0xB0)** sinyali kullanılacaktır (Adım 5).

- **Amaç:** AKS'e UKS'in canlı/bağlı olduğunun periyodik teyidi
  (stabilizasyon teyidi).
- **Komut kanalından bağımsızdır:** 0xA1–0xA4 operatör tetiklemeli ayrık
  komutlardır; 0xB0 ise zamanlayıcı tetiklemeli, içerik taşımayan bir
  "canlılık" sinyalidir.
- **Durum: UYGULANDI.** `main.c` içinde, mevcut 3 saniyelik istatistik
  zamanlayıcısından (`last_heartbeat_ms`) ayrı, bağımsız bir
  `last_heartbeat_tx_ms` zamanlayıcısı eklendi. Her `LORA_HEARTBEAT_PERIOD_MS`
  (1000 ms, `lora.h`) bir kez, `Lora_Send()` ile AKS'e tek byte `0xB0`
  (`LORA_HEARTBEAT_BYTE`) gönderilir. Gönderim best-effort'tur: başarısız
  olursa sessizce atlanır ve bir sonraki periyotta tekrar denenir — E-STOP
  gibi kritik bir komut değildir, bu yüzden `Lora_SendCritical` değil
  `Lora_Send` kullanılır.

Bu tasarımla heartbeat, 9.2.a'nın izin verdiği "stabilizasyon teyidi geri
bildirimi" istisnasına girer ve kurala uygundur. UKS her ~1 saniyede bir
AKS'e `0xB0` byte'ı gönderir; bu, komut kanalından (0xA1-0xA4) bağımsızdır.
AKS tarafı bu byte'ı almazsa (kendi link-timeout eşiğine göre) link_down
durumuna geçer — UKS tarafında simetrik tespit `TEL_LINK_TIMEOUT_MS` (2000 ms,
`telemetry.h`) ile uygulanmıştır: bu süre boyunca geçerli `TEL` frame'i
gelmezse UKS `LINK,DOWN` durumuna geçer ve PC'ye/konsola bildirir; geçerli
bir frame geldiğinde `LINK,UP` ile geri döner.

**Doğrulanmadı:** Bu bölümdeki 9.2.a yorumu ekip tarafından yapılmış bir
okumadır; teknik kontrole girmeden önce danışmana/jüriye teyit
ettirilmelidir.

## 3. İzleme Merkezi = PC

- **İzleme merkezi PC'dir.** Telemetri verisi UKS üzerinden PC'ye iletilir
  ve operatör arayüzü PC'de çalışır.
- **UKS (STM32), aracın DIŞINDA** yer alan bir alıcı/köprü cihazıdır. Yer
  istasyonu tarafında konumlanır.
- **Araç tarafında (madde 9.2.i)** yalnızca **AKS (ESP32)** bulunur. PC,
  laptop veya telefon araç İÇİNDE yer almaz — bunlar yer istasyonu
  ekipmanıdır.

## 4. AÇIK SORU — ÇÖZÜLDÜ

Komut kanalı ve UKS E-STOP butonu 9.2.a uyumu için sistemden tamamen
kaldırıldı (2026-07-03). RF hattı tek yönlü telemetri + 0xB0 stabilizasyon
teyididir. Acil durdurma araç üstü fiziksel kontaktörledir, RF'ten
bağımsızdır.

## 5. Sayım Konvansiyonu — `TEL_FIELD_COUNT`

`TEL_FIELD_COUNT=19`, `"TEL"` etiketini alan #0 olarak DAHİL sayar.
Sayısal alan sayısı 18'dir (`ver`..`spd_x10`). "AKS 18 alan gönderiyor,
UKS 19 bekliyor" ifadesi bir uyumsuzluk DEĞİLDİR — iki farklı sayım
konvansiyonudur (biri etiketi sayar, diğeri saymaz). Alan sayısı
değişikliği önerilmeden önce token sayımı `TEL` dahil yapılmalıdır.

Bu not, aynı yanlış alarmın üçüncü kez yaşanmasını önlemek içindir.

## 6. Teknik Kontrol Checklist (Madde 9.2.i)

Teknik kontrol sırasında aşağıdakiler doğrulanmalıdır:

- [ ] Araç üzerinde **PC / laptop / telefon YOK**.
- [ ] Araç üzerinde **switch / modem YOK**.
- [ ] Hız göstergesi: **Nextion ekran (araç üzerinde)** + **PC (yer
      istasyonunda)**; bu ikisi dışında ek gösterge yok.
- [ ] **Telefon kullanılmıyor** (ne araçta ne yer istasyonunda telemetri/
      kontrol amaçlı).
- [ ] Telemetri göndericisi (araç tarafı) = **ESP32 (AKS) MCU**; başka bir
      gönderici MCU/modül yok.

---

*Bu not, mevcut kod tabanındaki protokol sabitlerine ve davranışına göre
hazırlanmıştır. Protokol değiştikçe (örn. 0xB0 uygulamaya alındığında) bu
dokümanın güncellenmesi gerekir.*
