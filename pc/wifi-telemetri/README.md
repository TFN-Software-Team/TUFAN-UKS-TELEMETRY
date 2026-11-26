# Wi-Fi Telemetri Sayfası (TEST yolu)

AKS ESP32'sinin Wi-Fi üzerinden yayınladığı 19 alanlı `TEL,2,...` satırlarını
canlı gösteren tek dosyalık sayfa. Kurulum gerektirmez; `index.html` dosyasını
tarayıcıda açmanız yeterli. **Resmi telemetri kanalı LoRa'dır**; bu sayfa
test/pit içindir. ESP32 tarafı: `TUFAN-AKS/Documents/WIFI_TELEMETRI_TEST.md`.

## İki kaynak modu

| Mod | Ne zaman | Ayarlar |
|---|---|---|
| **MQTT** | PC ESP32'den farklı bir ağda (ev/okul Wi-Fi'si vb.) | HiveMQ Cloud **WebSocket** adresi `wss://<cluster>.s1.eu.hivemq.cloud:8884/mqtt`, kullanıcı/şifre, konu kökü (ESP32'deki `MQTT_TEL_TOPIC_BASE`, örn. `tufan/aks`) |
| **Doğrudan ESP32** | PC de aynı telefon hotspot'unda | ESP32 IP'si (seri logda `Hotspot'a baglandi — IP ...` satırında ya da MQTT durumunda görünür) |

Doğrudan modda sayfa 0,5 sn'de bir `GET /telemetry.csv?since=<son seq>` ile
yeni satırları çeker. Kısa kopmalarda ESP32'deki son ~30 sn'lik tampondan
boşluksuz tamamlar.

## Kullanım

1. `index.html` dosyasını Chrome/Firefox'ta açın (çift tıklama yeterli).
   MQTT modu için internet gerekir; `mqtt.js` CDN'den yüklenir.
2. "Bağlantı ayarları" bölümünü doldurup **Bağlan**'a basın. Ayarlar tarayıcıda
   hatırlanır; şifre yalnızca "hatırla" işaretliyse saklanır.
3. **Oturum CSV indir**, bu oturumda alınan tüm ham satırları PC saatiyle
   birlikte indirir.

## Gösterilenler

Hız (`spd_x10/10`), paket gerilimi (`packV/10`), akım (`current/100` A, + şarj),
SoC, batarya sıcaklığı (H/L), hücre max/min (deci-mV → V), RPM, motor voltajı
(4. alan; sözleşmede adı "torque"), çalışma modu (sysState 1/2/3), geçerlilik
bayrakları, son 60 sn hız/akım grafiği, seq boşluğu (kayıp) sayacı ve ham
satırlar. 3 sn'den uzun süre satır gelmezse kartlar soluklaşır.

> Bu sayfa jüri kaydının (TUFAN-monitor, 9.2.f) yerini **tutmaz**.
