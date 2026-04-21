/**
 * @file    telemetry.h
 * @brief   Byte-stuffing destekli, cift-tamponlu telemetri + komut modulu.
 *
 *  Telemetri Paketi  (ham, 7 byte): [0xAA][speed][soc][temp][rpm_hi][rpm_lo][chk]
 *  Komut Paketi      (ham, 4 byte): [0xCC][cmd_id][param][chk]
 *
 *  Byte-Stuffing:
 *    0xAA -> 0xAB 0x01   (telemetri header rezerve)
 *    0xAB -> 0xAB 0x02   (escape byte rezerve)
 *    0xCC -> 0xAB 0x03   (komut header rezerve)
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H
#include <stdint.h>
#include <stddef.h>

/* ========== Protokol Sabitleri ========== */

/* Telemetri */
#define TEL_HEADER_BYTE         0xAA
#define TEL_ESC_BYTE            0xAB
#define TEL_ESC_HEADER          0x01
#define TEL_ESC_ESC             0x02
#define TEL_ESC_CMD             0x03
#define TEL_RAW_PACKET_LEN      7
#define TEL_PAYLOAD_LEN         6

/* Komut */
#define TEL_CMD_HEADER_BYTE     0xCC
#define TEL_CMD_RAW_LEN         4
#define TEL_CMD_PAYLOAD_LEN     3

/* E-STOP redundans */
#define TEL_ESTOP_BURST_COUNT   3
#define TEL_CMD_STUFFED_MAX_LEN (1 + TEL_CMD_PAYLOAD_LEN * 2)
#define TEL_ESTOP_BURST_MAX_LEN (TEL_CMD_STUFFED_MAX_LEN * TEL_ESTOP_BURST_COUNT)

/* Telemetri paketi indeksleri */
#define TEL_IDX_HEADER      0
#define TEL_IDX_SPEED       1
#define TEL_IDX_SOC         2
#define TEL_IDX_BAT_TEMP    3
#define TEL_IDX_RPM_HI      4
#define TEL_IDX_RPM_LO      5
#define TEL_IDX_CHK         6

/* Komut payload indeksleri */
#define TEL_CMD_IDX_ID      0
#define TEL_CMD_IDX_PARAM   1
#define TEL_CMD_IDX_CHK     2

/* Frame tipleri */
#define FRAME_TYPE_TELEMETRY    0
#define FRAME_TYPE_COMMAND      1

/* Aralik sinirlari */
#define TEL_SOC_MAX             100
#define TEL_BAT_TEMP_MAX        120
#define TEL_RPM_MAX             20000
#define TEL_PARTIAL_TIMEOUT_MS  50

/* ========== Veri Yapilari ========== */

typedef enum {
    TEL_VALID = 0,
    TEL_NO_DATA,
    TEL_CHK_FAIL,
    TEL_OUT_OF_RANGE,
    TEL_ERR_NULL,
    TEL_UNKNOWN_CMD
} TelStatus_t;

typedef enum {
    FRAME_IDLE = 0,
    FRAME_PAYLOAD,
    FRAME_ESC
} FrameState_t;

typedef enum {
    TEL_CMD_NONE         = 0x00,
    TEL_CMD_ESTOP        = 0x01,  /* Acil durdurma — latchlenir */
    TEL_CMD_ESTOP_CLEAR  = 0x02,  /* Uzaktan clear istegi (bilgi amacli) */
    TEL_CMD_PING         = 0x10
} TelCmd_t;

typedef struct {
    uint8_t  speed;
    uint8_t  soc;
    uint8_t  battery_temp;
    uint16_t motor_rpm;
} TelData_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_drop;
    uint32_t chk_fail;
    uint32_t range_fail;
    uint32_t timeout_drop;
    uint32_t stuff_err;
    uint32_t good_packets;
    uint32_t good_commands;
    uint32_t cmd_chk_fail;
    uint32_t estop_rx_count;
} TelStats_t;

typedef struct {
    uint8_t state;
    uint8_t idx;
    uint8_t type;           /* FRAME_TYPE_TELEMETRY | FRAME_TYPE_COMMAND */
    uint8_t expected_len;   /* bu frame'in toplam payload uzunlugu */
} FrameDecoder_t;

/** E-STOP alindi callback'i. ISR context'inde cagrilir — KISA TUTUN. */
typedef void (*TelEStopCb_t)(void *user);

typedef struct {
    FrameDecoder_t decoder;

    /* PING-PONG IKI KADEMELI TAMPON (TELEMETRI) */
    uint8_t          buffers[2][TEL_PAYLOAD_LEN];
    volatile uint8_t write_idx;
    volatile uint8_t read_idx;
    volatile uint8_t frame_ready;

    /* KOMUT TAMPONU — tek, komutlar nadir */
    uint8_t          cmd_buf[TEL_CMD_PAYLOAD_LEN];
    volatile uint8_t cmd_ready;

    /* E-STOP kilitli durum — ISR icinde aninda latchlenir */
    volatile uint8_t estop_active;
    TelEStopCb_t     estop_cb;
    void            *estop_cb_user;

    TelStats_t stats;
    uint32_t   last_rx_ms;
} TelCtx_t;

/* ========== Fonksiyon Prototipleri ========== */

/* Encoder */
uint8_t Telemetry_Encode       (const TelData_t *data, uint8_t *out_buf, size_t max_len);
uint8_t Telemetry_EncodeCommand(TelCmd_t cmd, uint8_t param,
                                uint8_t *out_buf, size_t max_len);
uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len);

/* Decoder / Parser */
void        Telemetry_Init         (TelCtx_t *ctx);
void        Telemetry_RxBytePush   (TelCtx_t *ctx, uint8_t rx_byte, uint32_t now_ms);
uint8_t     Telemetry_IsFrameReady (const TelCtx_t *ctx);
TelStatus_t Telemetry_Parse        (TelCtx_t *ctx, TelData_t *out);
void        Telemetry_Tick         (TelCtx_t *ctx, uint32_t now_ms);

/* Komut API */
uint8_t     Telemetry_IsCommandReady(const TelCtx_t *ctx);
TelStatus_t Telemetry_ParseCommand  (TelCtx_t *ctx, TelCmd_t *cmd_out, uint8_t *param_out);

/* E-STOP kontrolu */
uint8_t     Telemetry_IsEStopActive (const TelCtx_t *ctx);
void        Telemetry_ClearEStop    (TelCtx_t *ctx);
void        Telemetry_SetEStopCallback(TelCtx_t *ctx, TelEStopCb_t cb, void *user);

/* Istatistik / Ekran */
const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx);
void              Telemetry_ResetStats(TelCtx_t *ctx);
void              Telemetry_PrintCompact  (const TelData_t *data, TelStatus_t status);
void              Telemetry_PrintDashboard(const TelData_t *data, TelStatus_t status,
                                           uint8_t estop_active);
void              Telemetry_PrintStats    (const TelCtx_t *ctx);

#endif /* TELEMETRY_H */
