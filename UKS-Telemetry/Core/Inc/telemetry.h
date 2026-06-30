/**
 * @file    telemetry.h
 * @brief   AKS uyumlu ASCII CSV telemetri parser + tek-byte komut gondericisi.
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
 *        current        int32,  x0.01 mA (+sarj / -desarj)
 *        soc            uint16, x0.01 %  (0..10000 = %0.00..%100.00)
 *        bmsValid       0/1
 *        ts_ms          uint32, AKS boot'tan beri ms
 *        spd_x10        uint16, arac hizi x10 km/h
 *
 *      NOT: Bu format, ESP_AKS deposunda iki ayri calismanin (BMS alan
 *      bolunmesi + ts_ms/spd_x10 eklenmesi) BIRLESTIRILMESINI gerektirir;
 *      yazi yazildigi anda hicbir tek ESP_AKS dalinda 19 alanin TUMU bir
 *      arada uretilmiyor. UKS bu sozlesmeye gore hazir; AKS tarafinin da
 *      ayni birlesimi yapmasi gerekir.
 *
 *  UKS -> AKS Komut (tek byte, framing/checksum YOK):
 *      0xA1 = EMERGENCY_STOP
 *      0xA2 = START
 *      0xA3 = STOP
 *      0xA4 = DRIVE_ENABLE
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
 *  - E-STOP UKS'te lokal latch'lenir (AKS bu komutu UKS'e yansitmaz).
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
 * sarma). Air-rate ~450 ms/frame, en kotu bloklama (dashboard) ~60 ms →
 * pratikte 1 frame bile birikmiyor; 4 rahat marj birakir. */
#define TEL_FRAME_Q_DEPTH       4U
#define TEL_FRAME_Q_MASK        (TEL_FRAME_Q_DEPTH - 1U)

/* UKS -> AKS komut byte'lari (UKS_LoRa_Protocol.md ile birebir) */
#define UKS_CMD_EMERGENCY_STOP  0xA1U
#define UKS_CMD_START           0xA2U
#define UKS_CMD_STOP            0xA3U
#define UKS_CMD_DRIVE_ENABLE    0xA4U

/* E-STOP burst — paket kaybina karsi N kez ardisik gonderilir.
 * AKS taraf tek-byte bekledigi icin ardisik 3x 0xA1 da herhangi bir
 * RX okumasinda E-STOP olarak yorumlanir. */
#define TEL_ESTOP_BURST_COUNT   3U

/* Sanity araliklari (parser'da hard reject) */
#define TEL_RPM_MAX             20000

/* Link-down esigi: bu suredir gecerli TEL frame'i gelmediyse baglanti
 * kopmus sayilir (bkz. main.c link_down state, UYUM_NOTU.md bolum 2). */
#define TEL_LINK_TIMEOUT_MS     2000U

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
    int32_t   bms_current_centima;    /* x0.01 mA (+sarj / -desarj) */
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
    uint32_t estop_tx_count;   /* Operatorun tetikledigi E-STOP burst sayisi */
} TelStats_t;

/** UKS lokal E-STOP callback'i. ISR'dan da cagrilabilir; KISA TUTUN. */
typedef void (*TelEStopCb_t)(void *user);

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

    /* UKS lokal E-STOP durumu (operator butona basinca latchlenir) */
    volatile uint8_t estop_active;
    TelEStopCb_t     estop_cb;
    void            *estop_cb_user;

    TelStats_t stats;
    uint32_t   last_rx_ms;       /* son byte'in ana donguda islenme zamani */
} TelCtx_t;

/* ========== Encoder (UKS -> AKS) ========== */

/** Tek byte komut yazar. AKS framing/CRC beklemiyor. */
uint8_t Telemetry_EncodeCommand(uint8_t cmd_byte,
                                uint8_t *out_buf, size_t max_len);

/** N x 0xA1 burst yazar. Donus: yazilan byte sayisi (=N). */
uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len);

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

/* ========== UKS Lokal E-STOP ========== */

uint8_t     Telemetry_IsEStopActive (const TelCtx_t *ctx);
void        Telemetry_ClearEStop    (TelCtx_t *ctx);

/** Operator butona bastiginda cagrilir. Idempotent; yalnizca ilk
 *  cagrida callback tetiklenir. ISR-safe. */
void        Telemetry_SetEStopActive(TelCtx_t *ctx);

void        Telemetry_SetEStopCallback(TelCtx_t *ctx,
                                       TelEStopCb_t cb, void *user);

/* ========== Istatistik & Ekran ========== */

const TelStats_t *Telemetry_GetStats  (const TelCtx_t *ctx);
void              Telemetry_ResetStats(TelCtx_t *ctx);

void              Telemetry_PrintDashboard(const TelData_t *data,
                                           TelStatus_t status,
                                           uint8_t estop_active,
                                           uint8_t link_down);
void              Telemetry_PrintStats    (const TelCtx_t *ctx);

#endif /* TELEMETRY_H */