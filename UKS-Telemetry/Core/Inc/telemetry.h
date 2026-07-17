/**
 * @file    telemetry.h
 * @brief   AKS uyumlu ASCII CSV telemetri parser.
 *
 *  AKS -> UKS Telemetri v2 (her satir bir frame, CRLF sonlu):
 *      "TEL,<ver>,<seq>,<rpm>,<torque>,<motorErr>,<motorValid>,<motorTimeout>,
 *       <cellVMax>,<cellVMin>,<tempH>,<tempL>,<sysState>,
 *       <packV>,<current>,<soc>,<bmsValid>,<ts_ms>,<spd_x10>\r\n"
 *      Toplam 19 alan, ilk alan literal "TEL" (ESP_AKS lib/Telemetry ile
 *      birebir — Telemetry.cpp::sendStatus format string sirasi).
 *
 *      Olcekler (AKS Telemetry.h / TelemetryData ile birebir):
 *        rpm            uint16, ham RPM
 *        torque         int16,  ham
 *        motorErr       uint8,  bit bayrak
 *        motorValid/Timeout  0/1
 *        cellVMax/Min   uint16, x0.1 mV
 *        tempH/tempL    int8 kaynak (burada int16 saklanir), derece C
 *        sysState       uint8, 1=Discharge 2=IDLE 3=Charge 4=FAULT
 *        packV          uint16, x0.1 V
 *        current        int32,  x0.01 A (centi-Amper) (+sarj / -desarj)
 *                       (ONCEDEN birim yanlislikla 1000 kat kucuk yaziliyordu
 *                       (miliamper zannedilmisti) — AKS TEL_bmsCurrentCentiA
 *                       ile AYNI birim, bkz. AKS Telemetry.h sendStatus birim
 *                       sozlesmesi yorumu / Documents/UKS_LoRa_Protocol.md)
 *        soc            uint16, x0.01 %  (0..10000 = %0.00..%100.00)
 *        bmsValid       0/1
 *        ts_ms          uint32, AKS boot'tan beri ms
 *        spd_x10        uint16, arac hizi x10 km/h
 *
 *      NOT (cozuldu): Bu format onceden ESP_AKS deposunda iki ayri dalda
 *      (BMS alan bolunmesi + ts_ms/spd_x10 eklenmesi) durumdaydi; bu artik
 *      gecerli degil. Format ESP_AKS tarafinda
 *      lib/Telemetry/Telemetry.cpp::sendStatus icinde tek parcada
 *      uretiliyor ve golden fixture'larla dogrulaniyor (ESP_AKS
 *      test/test_native_telemetry/test_telemetry_format.cpp,
 *      tools/e2e/test_frame_contract.py).
 *
 *  9.2.a: RF hatti tek yonlu telemetri + heartbeat'tir (bkz. lora.h). UKS ->
 *  AKS komut kanali (eski 0xA1-0xA4) sistemden tamamen kaldirildi; acil
 *  durdurma arac ustundeki fiziksel kontaktorle saglanir, RF'ten bagimsizdir.
 *
 *  Mimari (v3/v4 — ISR yuku azaltildi):
 *  - ISR (Telemetry_RxBytePush) artik SADECE ham byte'i dairesel tampona
 *    (ring buffer) yazar ve cikar. Hicbir parse/tokenize ISR'da yapilmaz.
 *    Boylece ISR suresi her byte icin sabit ve mikrosaniye seviyesinde
 *    kalir → 8 MHz HSI'da bile Overrun (ORE) riski pratikte sifirlanir.
 *  - Satir birlestirme + parse + range check + commit, ana donguden
 *    cagrilan Telemetry_Process() icinde (main context) yapilir.
 *  - Decode edilmis frame'ler SPSC kuyruguna (frame_q) yazilir; Process
 *    uretici, Telemetry_Parse tuketicidir. Tek main turunda birden cok
 *    frame gelse de kuyruk derinligi sayesinde kaybolmaz.
 *  - Sequence numarasi izlenir; gap/duplicate sayilari stats'a yazilir.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stddef.h>

/* ========== Protokol Sabitleri ========== */

/* Telemetri (ASCII CSV) */
#define TEL_TAG_STR             "TEL"
#define TEL_TAG_LEN             3U
#define TEL_FIELD_COUNT         19U
#define TEL_LINE_MAX_LEN        128U     /* CRLF dahil maksimum satir
                                           * (19 alanli en kotu durum ~107
                                           * byte; 128 marjli sigar) */
#define TEL_PARTIAL_TIMEOUT_MS  500U     /* Yarim satirin atilma suresi */
#define TEL_PROTOCOL_VERSION    2U

/* RX ring buffer boyutu. 2'nin kuvveti olmali (maske ile sarma).
 * 256 byte: 9600 baud'da ~266 ms'lik veriyi tamponlar — ana dongu
 * dashboard basarken (60 ms) bile rahatca yetisilir. */
#define TEL_RX_RING_SIZE        256U
#define TEL_RX_RING_MASK        (TEL_RX_RING_SIZE - 1U)

/* Decode edilmis frame kuyrugu derinligi. 2'nin kuvveti olmali (maske ile
 * sarma). AKS tarafi link flapping duzeltmesiyle 5 Hz'den 2 Hz'e indi
 * (LORA_TX_PERIOD_MS=500, bkz. AKS SystemConfig.h); nominal frame araligi
 * ~500 ms, en kotu bloklama (dashboard) ~60 ms → pratikte 1 frame bile
 * birikmiyor; 4 rahat marj birakir. */
#define TEL_FRAME_Q_DEPTH       4U
#define TEL_FRAME_Q_MASK        (TEL_FRAME_Q_DEPTH - 1U)

/* Sanity araliklari (parser'da hard reject) */
#define TEL_RPM_MAX             20000

/* Link-down esigi: bu suredir gecerli TEL frame'i gelmediyse baglanti
 * kopmus sayilir (bkz. main.c link_down state, UYUM_NOTU.md bolum 2).
 * AKS tarafi 2.4 kbps hava hizi kalibrasyonuyla 1 Hz'e indi
 * (LORA_TX_PERIOD_MS=1000, bkz. AKS SystemConfig.h); nominal frame araligi
 * ~1000 ms. Eski deger (2000U) eski periyotla (500 ms) tam 4x marj
 * tasiyordu; ayni 4x oran AYNI COMMIT'te korunarak 4000U'ya yeniden
 * kalibre edildi (>= 3x LORA_TX_PERIOD_MS invaryanti icin bkz.
 * tools/e2e/test_contract_drift.py::
 * test_uks_tel_link_timeout_has_enough_margin_over_tx_period). */
#define TEL_LINK_TIMEOUT_MS     4000U

/* ========== Tipler ========== */

typedef enum {
    TEL_VALID = 0,
    TEL_NO_DATA,
    TEL_ERR_NULL
} TelStatus_t;

typedef enum {
    LINE_IDLE = 0,
    LINE_COLLECT,
    LINE_OVERFLOW   /* tampon tasti: \n gorene kadar gelen her byte cop */
} LineState_t;

/**
 * @brief Tek bir telemetri frame'i. Tum sayisal alanlar AKS dokumaninda
 *        belirtilen olcek ve isaretlilikte tutulur (ham degerler).
 */
typedef struct {
    /* Frame kontrol */
    uint8_t   protocol_version;
    uint32_t  sequence;

    /* Motor */
    uint16_t  motor_rpm;
    int16_t   motor_torque;
    uint8_t   motor_error_flags;
    uint8_t   motor_data_valid;
    uint8_t   motor_timeout_active;

    /* BMS (Solion SK BMS uyumlu — ESP_AKS Telemetry.h v2 alan bolunmesi) */
    uint16_t  bms_cell_vmax_decimv;   /* x0.1 mV */
    uint16_t  bms_cell_vmin_decimv;   /* x0.1 mV */
    int16_t   bms_temp_highest_c;     /* derece C (kaynak int8, burada int16) */
    int16_t   bms_temp_lowest_c;      /* derece C */
    uint8_t   bms_system_state;       /* 1=Discharge 2=IDLE 3=Charge 4=FAULT */
    uint16_t  bms_pack_voltage_deciv; /* x0.1 V */
    /* NOT: alan adi "centima" (centi-mA cagristirir) ama gercek birim
     * centi-Amper (0.01 A) — AKS TEL_bmsCurrentCentiA ile AYNI. Alan ADI
     * ABI/wire-format'in bir parcasi DEGIL (yalnizca bu struct'in ic
     * kullanimi), bu yuzden yaniltici olsa da wire uyumunu BOZMADAN
     * "bms_current_centiamp" gibi bir isme yeniden adlandirilabilir —
     * ancak bu yeniden adlandirma bu degisiklikte YAPILMADI (telemetry.c
     * + test/native/test_telemetry_v2.c gibi tum kullanim yerlerini
     * etkiler); asgari duzeltme olarak BIRIM yorumu asagida dogru: */
    int32_t   bms_current_centima;    /* x0.01 A (centi-Amper) (+sarj / -desarj) */
    uint16_t  bms_soc_hundredths;     /* x0.01 % (0..10000 = %0.00..%100.00) */
    uint8_t   bms_data_valid;

    /* Zaman / hiz (v2 ile eklendi) */
    uint32_t  timestamp_ms;           /* AKS boot'tan beri ms */
    uint16_t  speed_kmh_x10;          /* arac hizi x10 km/h */
} TelData_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_lines;
    uint32_t parse_fail;       /* Field sayisi/format hatasi */
    uint32_t bad_tag;          /* Ilk alan "TEL" degil */
    uint32_t bad_version;
    uint32_t range_fail;
    uint32_t timeout_drop;        /* Yarim satir timeout */
    uint32_t line_overflow_drop;  /* Satir tamponu doldu (128 B limit asildi) */
    uint32_t queue_overflow_drop; /* Frame kuyrugu dolu (ana dongu yetisemedi) */
    uint32_t ring_overflow;       /* RX ring buffer doldu (ISR yetisemedi) */
    uint32_t good_packets;
    uint32_t seq_gaps;         /* Beklenenin uzerinde atlama */
    uint32_t seq_dup_or_stale; /* Ayni veya geri giden sira */
} TelStats_t;

typedef struct {
    /* ---- RX ring buffer (ISR write, main read) ----
     * ISR sadece head'e yazar; ana dongu (Telemetry_Process) tail'den okur.
     * Tek-uretici/tek-tuketici → kilit gerekmez (head volatile yeterli). */
    uint8_t          rx_ring[TEL_RX_RING_SIZE];
    volatile uint16_t rx_head;   /* ISR yazar */
    volatile uint16_t rx_tail;   /* ana dongu okur — SPSC tutarliligi */

    /* ASCII satir parser tamponu (artik ana donguden kullanilir) */
    LineState_t line_state;
    uint16_t    line_len;
    uint8_t     line_buf[TEL_LINE_MAX_LEN];

    /* Decode edilmis frame kuyrugu (SPSC). Uretici = Telemetry_Process
     * (Commit_Frame), tuketici = Telemetry_Parse. rx_ring ile birebir ayni
     * desen — yalnizca uint8_t yerine TelData_t tasir. Tek-uretici/tek-
     * tuketici oldugu icin kilit GEREKMEZ (head/tail volatile yeterli). */
    TelData_t        frame_q[TEL_FRAME_Q_DEPTH];
    volatile uint8_t fq_head;   /* Commit_Frame yazar (yayinlar) */
    volatile uint8_t fq_tail;   /* Telemetry_Parse yazar (tuketir) */

    /* Sequence izleme */
    uint32_t  last_sequence;
    uint8_t   have_last_seq;

    TelStats_t stats;
    uint32_t   last_rx_ms;       /* son byte'in ana donguda islenme zamani */
} TelCtx_t;

/* ========== Decoder (AKS -> UKS) ========== */

void        Telemetry_Init        (TelCtx_t *ctx);

/** RX byte'ini ISR icinden besle. SADECE ring buffer'a yazar — parse YAPMAZ.
 *  ISR-safe, sabit-zamanli (mikrosaniye). */
void        Telemetry_RxBytePush  (TelCtx_t *ctx, uint8_t rx_byte, uint32_t now_ms);

/** Ring buffer'daki bekleyen byte'lari isler: satir birlestirme + parse +
 *  range check + commit. ANA DONGUDEN periyodik cagir (main context).
 *  Tum agir is burada yapilir; ISR yuku minimumda tutulur. */
void        Telemetry_Process     (TelCtx_t *ctx, uint32_t now_ms);

uint8_t     Telemetry_IsFrameReady(const TelCtx_t *ctx);

/** Hazir frame'i out'a kopyalar. Hazir frame yoksa TEL_NO_DATA. */
TelStatus_t Telemetry_Parse       (TelCtx_t *ctx, TelData_t *out);

/** Yarim satir varsa timeout ile iptal eder. Periyodik cagir. */
void        Telemetry_Tick        (TelCtx_t *ctx, uint32_t now_ms);

/* ========== Istatistik & Ekran ========== */

const TelStats_t *Telemetry_GetStats  (const TelCtx_t *ctx);
void              Telemetry_ResetStats(TelCtx_t *ctx);

void              Telemetry_PrintDashboard(const TelData_t *data,
                                           TelStatus_t status,
                                           uint8_t link_down);
void              Telemetry_PrintStats    (const TelCtx_t *ctx);

#endif /* TELEMETRY_H */