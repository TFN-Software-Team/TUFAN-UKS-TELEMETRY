/**
 * @file    telemetry.h
 * @brief   LoRa uzerinden gelen telemetri paketlerini
 *          byte-stuffing + durum makinesi ile alip parse eden modul.
 *
 *  Ham (stuffing oncesi) Paket Formati — 7 byte:
 *  [0] 0xAA        — Header  (hatta sadece header olarak gecebilir)
 *  [1] speed       — km/h
 *  [2] soc         — %
 *  [3] bat_temp    — C
 *  [4] rpm_hi      — Motor RPM ust byte
 *  [5] rpm_lo      — Motor RPM alt byte
 *  [6] checksum    — XOR of bytes [0..5]
 *
 *  Byte-Stuffing Kurallari (hatta giden veri):
 *  - 0xAA veri icinde gecemez; header icin rezervedir.
 *  - Veri byte'i 0xAA ise  ->  0xAB 0x01 olarak gonderilir.
 *  - Veri byte'i 0xAB ise  ->  0xAB 0x02 olarak gonderilir.
 *  - Diger byte'lar        ->  oldugu gibi gonderilir.
 *
 *  Hatta giden paket:
 *  [0xAA] [stuffed payload: bytes 1..6]
 *  Header asla stuff edilmez, sadece payload (6 byte) stuff edilir.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

/* ---- Yapilandirma Sabitleri ---- */

#ifndef TEL_RX_BUF_SIZE
#define TEL_RX_BUF_SIZE         256U
#endif

#define TEL_RAW_PACKET_LEN      7U     /* stuffing oncesi paket boyu    */
#define TEL_PAYLOAD_LEN         6U     /* header haric payload (1..6)   */
#define TEL_HEADER_BYTE         0xAAU
#define TEL_ESC_BYTE            0xABU  /* kacis (escape) byte'i         */
#define TEL_ESC_HEADER          0x01U  /* 0xAB 0x01 -> gercek 0xAA     */
#define TEL_ESC_ESC             0x02U  /* 0xAB 0x02 -> gercek 0xAB     */

/** Kismi paket icin timeout (ms) */
#ifndef TEL_PARTIAL_TIMEOUT_MS
#define TEL_PARTIAL_TIMEOUT_MS  200U
#endif

/** Stuffed paketin max boyu: header(1) + payload*2 (worst case) */
#define TEL_STUFFED_MAX_LEN     (1U + TEL_PAYLOAD_LEN * 2U)

/* ---- Alan Sinirlari (Range Check) ---- */

#define TEL_SPEED_MAX           255U
#define TEL_SOC_MAX             100U
#define TEL_BAT_TEMP_MAX        80U
#define TEL_RPM_MAX             15000U

/* ---- Paket Alan Indeksleri (ham paket, stuffing oncesi) ---- */

enum {
    TEL_IDX_HEADER   = 0,
    TEL_IDX_SPEED    = 1,
    TEL_IDX_SOC      = 2,
    TEL_IDX_BAT_TEMP = 3,
    TEL_IDX_RPM_HI   = 4,
    TEL_IDX_RPM_LO   = 5,
    TEL_IDX_CHK      = 6
};

/* ---- Donus Durumlari ---- */

typedef enum {
    TEL_VALID        = 0,
    TEL_NO_DATA,
    TEL_CHK_FAIL,
    TEL_OUT_OF_RANGE
} TelStatus_t;

/* ---- Telemetri Veri Struct'i ---- */

typedef struct {
    uint8_t  speed;
    uint8_t  soc;
    uint8_t  battery_temp;
    uint16_t motor_rpm;
} TelData_t;

/* ---- Hata / Istatistik Sayaclari ---- */

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_drop;
    uint32_t chk_fail;
    uint32_t range_fail;
    uint32_t timeout_drop;
    uint32_t stuff_err;      /**< Gecersiz escape dizisi sayisi */
    uint32_t good_packets;
} TelStats_t;

/* ---- Frame Decoder Durum Makinesi ---- */

typedef enum {
    FRAME_IDLE,              /**< Header bekleniyor                  */
    FRAME_PAYLOAD,           /**< Payload byte'lari aliniyor         */
    FRAME_ESC                /**< Escape alindi, sonraki bekleniyor  */
} FrameState_t;

typedef struct {
    FrameState_t state;
    uint8_t      payload[TEL_PAYLOAD_LEN];
    uint8_t      idx;
} FrameDecoder_t;

/* ---- Alici Baglami (coklu instance) ---- */

typedef struct {
    uint8_t          frame_buf[TEL_PAYLOAD_LEN];
    volatile uint8_t frame_ready;

    FrameDecoder_t   decoder;
    uint32_t         last_rx_ms;
    TelStats_t       stats;
} TelCtx_t;

/* ================================================================ */
/*                          PUBLIC API                              */
/* ================================================================ */

void        Telemetry_Init(TelCtx_t *ctx);

/* ---- Gonderici (Encoder) ---- */

uint8_t     Telemetry_Encode(const TelData_t *data, uint8_t *out_buf);

/* ---- Alici ---- */

void        Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t rx_byte);
uint8_t     Telemetry_IsFrameReady(const TelCtx_t *ctx);
TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out);
void        Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms);

/* ---- Istatistik ---- */

const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx);
void              Telemetry_ResetStats(TelCtx_t *ctx);

/* ---- Yer Istasyonu Ekrani ---- */

void Telemetry_PrintCompact(const TelData_t *data, TelStatus_t status);
void Telemetry_PrintDashboard(const TelData_t *data, TelStatus_t status);
void Telemetry_PrintStats(const TelCtx_t *ctx);

#endif /* TELEMETRY_H */
