# Wi-Fi Telemetri — PC tarafı (TEST yolu)

AKS ESP32'sinin (veya test kartı ESP32-S3'ün) Wi-Fi üzerinden yayınladığı
19 alanlı `TEL,2,...` satırlarını **kalıcı kaydeden** (logger) ve **gösteren**
(sayfa + TUFAN-monitor) araçlar. **Resmi telemetri kanalı LoRa'dır**; bu yol
test/pit içindir ve jüri kaydının (TUFAN-monitor, LoRa→UKS, 9.2.f) yerini
**tutmaz**. ESP32 tarafı: `TUFAN-AKS/Documents/WIFI_TELEMETRI_TEST.md`.

## Zincir

```
ESP32 ──(telefon hotspot'u)──> İnternet ──MQTT/TLS──> HiveMQ Cloud
                                                          │
   logger.py  <───────── abone olur (tufan/aks/tel) ──────┘
     ├─ logs/wifi_YYYYMMDD_HHMMSS.csv     kalıcı kayıt (her satır anında diske)
     ├─ logs/wifi_events_*.log            bağlantı/hata olayları
     └─ http://<pc>:8080                  kayıtlı verinin web adresi
          ├─ /                 canlı sayfa (index.html, "PC Logger" modu)
          ├─ /api/lines?since= kayıtlı satırlar (JSON)  ──> TUFAN-monitor --port WIFI
          ├─ /api/status       logger + ESP32 durumu
          ├─ /api/logs         log dosyaları
          └─ /logs/<dosya>     log indirme
```

Neden logger var: HiveMQ veriyi yalnız **iletir, saklamaz**; tarayıcı
sayfası da yalnız açıkken gelenleri görür. Logger, arayüzlerden bağımsız
olarak her satırı diske yazar; arayüzler (sayfa, TUFAN-monitor) veriyi
logger'dan çeker.

## Kurulum

```
pip install -r requirements.txt          # paho-mqtt (+ testler için pytest)
cp logger_ayarlar.example.json logger_ayarlar.json   # git'e GİRMEZ
```

`logger_ayarlar.json` içine HiveMQ cluster adresini (`broker`, `wss://` veya
`mqtts://` öneki OLMADAN), kullanıcı adını ve konu kökünü yazın. Şifreyi
dosyaya yazmak yerine ortam değişkeniyle verebilirsiniz:
`TUFAN_MQTT_PASSWORD=... python logger.py`.

## Çalıştırma

```
python logger.py                       # ayarlar logger_ayarlar.json'dan
python logger.py --source esp --esp http://192.168.43.120   # aynı hotspot'tayken, broker'sız
python logger.py --listen 0.0.0.0      # aynı ağdaki başka PC'ler de bağlansın (kimlik doğrulama YOK)
```

Ardından:

- **Sayfa:** tarayıcıda `http://localhost:8080` → otomatik "PC Logger"
  modunda açılır; oturumun tüm kayıtlı geçmişini yükler, sonra canlı
  devam eder. Kayıtlı log dosyaları sayfanın altından indirilebilir.
- **TUFAN-monitor (UKS arayüzü):** `python monitor.py --port WIFI`
  (bkz. TUFAN-monitor README, "Wi-Fi TEST kaynağı"). Dosyalar `_WIFI`
  ekiyle yazılır, jüri kaydıyla karışmaz.

Logger'ı Ctrl+C ile durdurun; dosyalar kapatılır. Her açılış yeni bir
`wifi_*.csv` başlatır.

## Kayıt dosyası biçimi

```
no;pc_zaman;pc_ms;tel_satiri
1;2026-11-27T15:00:22.311;1795780822311;TEL,2,0,0,791,0,1,0,33198,33076,32,29,1,795,-88,9000,1,0,0
```

`no` logger'ın sıra numarası, `pc_zaman` PC'nin alış saati; `tel_satiri`
ESP32'nin gönderdiği satırın aynısı (alanlar: `index.html` FIELDS ve
AKS `Telemetry::formatFrame`). Aynı satır ikinci kez gelirse (MQTT yeniden
gönderimi) yazılmaz; seq boşlukları ve ESP32 yeniden başlamaları sayılır
ve olay günlüğüne yazılır.

## Sayfanın diğer modları

| Mod | Ne zaman |
|---|---|
| **PC Logger** (varsayılan) | Logger çalışıyorken — kayıtlı geçmiş + canlı |
| **MQTT** | Logger yokken doğrudan HiveMQ'ya WebSocket ile (`wss://<cluster>:8884/mqtt`); `index.html` dosyasını çift tıklayıp açmak yeterli |
| **Doğrudan ESP32** | PC de aynı hotspot'tayken ESP32'nin `/telemetry.csv` uç noktasından |

Gösterilenler: hız (`spd_x10/10`), paket gerilimi (`packV/10`), akım
(`current/100` A, + şarj), SoC, batarya sıcaklığı (H/L), hücre max/min
(deci-mV → V), RPM, motor voltajı (4. alan; sözleşmede adı "torque"),
çalışma modu (sysState 1/2/3), geçerlilik bayrakları, son 60 sn hız/akım
grafiği, seq boşluğu sayacı ve ham satırlar. 3 sn'den uzun süre satır
gelmezse kartlar soluklaşır. S3 test kartı sahte veri üretiyorsa üst
bantta "SAHTE VERİ (S3 simülatör)" yazar.

## Testler

```
python -m pytest -q tests
```
