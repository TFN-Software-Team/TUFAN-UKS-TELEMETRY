/**
 * @file    telemetry.h
 * @brief   AKS uyumlu ASCII CSV telemetri parser + tek-byte komut gondericisi.
 *
 *  AKS -> UKS Telemetri (her satir bir frame, CRLF sonlu):
 *      "TEL,<ver>,<seq>,<rpm>,<torq>,<merr>,<mvalid>,<mtout>,
 *       <soc>,<bcurr>,<btemp>,<bvolt>,<bcell>,<berr>,<bvalid>\r\n"
 *      Toplam 15 alan, ilk alan literal "TEL".
 *
 *  UKS -> AKS Komut (tek byte, framing/checksum YOK):
 *      0xA1 = EMERGENCY_STOP
 *      0xA2 = START
 *      0xA3 = STOP
 *      0xA4 = DRIVE_ENABLE
 *
 *  Mimari:
 *  - Cift tampon (ping-pong) TelData_t — ISR yazarken ana dongu okur.
 *  - Satir tamponu tek (line_buf) — ISR icinde byte byte doldurulur.
 *    Satir tamamlaninca (\n) ISR icinde parse + range check + commit.
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
#define TEL_FIELD_COUNT         15U
#define TEL_LINE_MAX_LEN        128U     /* CRLF dahil maksimum satir */
#define TEL_PARTIAL_TIMEOUT_MS  500U     /* Yarim satirin atilma suresi */
#define TEL_PROTOCOL_VERSION    1U

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
#define TEL_BMS_SOC_MAX         100
#define TEL_BMS_TEMP_MAX        120
#define TEL_BMS_TEMP_MIN        (-40)
#define TEL_RPM_MAX             20000

/* ========== Tipler ========== */

typedef enum {
    TEL_VALID = 0,
    TEL_NO_DATA,
    TEL_ERR_NULL
} TelStatus_t;

typedef enum {
    LINE_IDLE = 0,
    LINE_COLLECT
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

    /* BMS */
    uint8_t   bms_soc;             /* yuzde 0..100 */
    int16_t   bms_current_dA;      /* deci-amper (1 = 0.1 A), isaretli */
    int16_t   bms_temp_C;          /* derece C, isaretli */
    uint16_t  bms_pack_voltage_dV; /* deci-volt (1 = 0.1 V) */
    uint16_t  bms_avg_cell_mV;     /* mV */
    uint8_t   bms_error_flags;
    uint8_t   bms_data_valid;
} TelData_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_lines;
    uint32_t parse_fail;       /* Field sayisi/format hatasi */
    uint32_t bad_tag;          /* Ilk alan "TEL" degil */
    uint32_t bad_version;
    uint32_t range_fail;
    uint32_t timeout_drop;     /* Yarim satir timeout */
    uint32_t overflow_drop;    /* Satir tamponu doldu / frame slot dolu */
    uint32_t good_packets;
    uint32_t seq_gaps;         /* Beklenenin uzerinde atlama */
    uint32_t seq_dup_or_stale; /* Ayni veya geri giden sira */
    uint32_t estop_tx_count;   /* Operatorun tetikledigi E-STOP burst sayisi */
} TelStats_t;

/** UKS lokal E-STOP callback'i. ISR'dan da cagrilabilir; KISA TUTUN. */
typedef void (*TelEStopCb_t)(void *user);

typedef struct {
    /* ASCII satir parser tamponu */
    LineState_t line_state;
    uint16_t    line_len;
    uint8_t     line_buf[TEL_LINE_MAX_LEN];

    /* Cift tampon — ISR write, main read */
    TelData_t        buffers[2];
    volatile uint8_t write_idx;
    volatile uint8_t read_idx;
    volatile uint8_t frame_ready;

    /* Sequence izleme */
    uint32_t  last_sequence;
    uint8_t   have_last_seq;

    /* UKS lokal E-STOP durumu (operator butona basinca latchlenir) */
    volatile uint8_t estop_active;
    TelEStopCb_t     estop_cb;
    void            *estop_cb_user;

    TelStats_t stats;
    uint32_t   last_rx_ms;
} TelCtx_t;

/* ========== Encoder (UKS -> AKS) ========== */

/** Tek byte komut yazar. AKS framing/CRC beklemiyor. */
uint8_t Telemetry_EncodeCommand(uint8_t cmd_byte,
                                uint8_t *out_buf, size_t max_len);

/** N x 0xA1 burst yazar. Donus: yazilan byte sayisi (=N). */
uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len);

/* ========== Decoder (AKS -> UKS) ========== */

void        Telemetry_Init        (TelCtx_t *ctx);

/** RX byte'ini ISR icinden besle. Satir tamamlandiginda parse + commit
 *  ayni cagri icinde yapilir. */
void        Telemetry_RxBytePush  (TelCtx_t *ctx, uint8_t rx_byte, uint32_t now_ms);

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
                                           uint8_t estop_active);
void              Telemetry_PrintStats    (const TelCtx_t *ctx);

#endif /* TELEMETRY_H */
