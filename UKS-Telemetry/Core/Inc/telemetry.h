#ifndef __TELEMETRY_H
#define __TELEMETRY_H

#include <stdint.h>
#include <stddef.h>

/* ========== Protokol Sabitleri ========== */
#define TEL_HEADER_BYTE         0xAA
#define TEL_CMD_HEADER_BYTE     0xCC
#define TEL_ESC_BYTE            0xAB

#define TEL_ESC_HEADER          0x01
#define TEL_ESC_ESC             0x02
#define TEL_ESC_CMD             0x03

/* ========== Veri Boyutları ve Sınırları ========== */
#define TEL_RAW_PACKET_LEN      7   // Header(1) + Payload(5) + Checksum(1)
#define TEL_PAYLOAD_LEN         6   // Speed(1) + SOC(1) + Temp(1) + RPM(2) + Checksum(1)

#define TEL_CMD_RAW_LEN         4   // Header(1) + Cmd(1) + Param(1) + Checksum(1)
#define TEL_CMD_PAYLOAD_LEN     3   // Cmd(1) + Param(1) + Checksum(1)

#define TEL_CMD_STUFFED_MAX_LEN 8   // Byte-stuffing sonrası max komut boyutu
#define TEL_ESTOP_BURST_COUNT   3
#define TEL_ESTOP_BURST_MAX_LEN (TEL_ESTOP_BURST_COUNT * TEL_CMD_STUFFED_MAX_LEN)

#define TEL_PARTIAL_TIMEOUT_MS  500 // Yarım kalan paketler için zaman aşımı

/* Sensör sınır değerleri (Hatalı verileri elemek için) */
#define TEL_SOC_MAX             100
#define TEL_BAT_TEMP_MAX        100
#define TEL_RPM_MAX             20000

/* Dizin (Index) Sabitleri */
#define TEL_IDX_HEADER          0
#define TEL_IDX_SPEED           1
#define TEL_IDX_SOC             2
#define TEL_IDX_BAT_TEMP        3
#define TEL_IDX_RPM_HI          4
#define TEL_IDX_RPM_LO          5
#define TEL_IDX_CHK             6

#define FRAME_TYPE_TELEMETRY    1
#define FRAME_TYPE_COMMAND      2

/* ========== Veri Yapıları (Enums & Structs) ========== */

// Durum (State Machine) Tipleri
typedef enum {
    FRAME_IDLE = 0,
    FRAME_PAYLOAD,
    FRAME_ESC
} FrameState_t;

// İşlem Sonuç Durumları
typedef enum {
    TEL_VALID = 0,
    TEL_NO_DATA,
    TEL_CHK_FAIL,
    TEL_OUT_OF_RANGE,
    TEL_ERR_NULL,
    TEL_UNKNOWN_CMD
} TelStatus_t;

// Komut Tipleri
typedef enum {
    TEL_CMD_PING        = 0x01,
    TEL_CMD_ESTOP       = 0x0E,
    TEL_CMD_ESTOP_CLEAR = 0x0F
} TelCmd_t;

// Sensör Veri Paketi
typedef struct {
    uint8_t  speed;
    uint8_t  soc;
    int8_t   battery_temp;
    uint16_t motor_rpm;
} TelData_t;

// Çerçeve (Frame) Çözücü
typedef struct {
    uint8_t type;
    uint8_t idx;
    uint8_t expected_len;
    FrameState_t state;
} FrameDecoder_t;

// İstatistik ve Hata Takibi
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

typedef void (*TelEStopCb_t)(void *user);

// Ana Telemetri Context Yapısı (Ping-Pong Buffer içerir)
typedef struct {
    FrameDecoder_t decoder;
    uint8_t buffers[2][TEL_PAYLOAD_LEN]; // Ping-Pong Çift Tampon
    uint8_t cmd_buf[TEL_CMD_PAYLOAD_LEN];
    uint8_t read_idx;
    uint8_t write_idx;
    volatile uint8_t frame_ready;
    volatile uint8_t cmd_ready;
    volatile uint8_t estop_active;
    uint32_t last_rx_ms;
    TelStats_t stats;
    TelEStopCb_t estop_cb;
    void *estop_cb_user;
} TelCtx_t;

/* ========== Fonksiyon Prototipleri ========== */

// Başlatma ve Veri Besleme
void Telemetry_Init(TelCtx_t *ctx);
void Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t rx_byte, uint32_t now_ms);
void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms);

// Bayrak Kontrolleri
uint8_t Telemetry_IsFrameReady(const TelCtx_t *ctx);
uint8_t Telemetry_IsCommandReady(const TelCtx_t *ctx);
uint8_t Telemetry_IsEStopActive(const TelCtx_t *ctx);

// Veri Çözme (Parse)
TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out);
TelStatus_t Telemetry_ParseCommand(TelCtx_t *ctx, TelCmd_t *cmd_out, uint8_t *param_out);

// Encode (Gönderim için Paketleme)
uint8_t Telemetry_Encode(const TelData_t *data, uint8_t *out_buf, size_t max_len);
uint8_t Telemetry_EncodeCommand(TelCmd_t cmd, uint8_t param, uint8_t *out_buf, size_t max_len);
uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len);

// E-STOP ve İstatistik Fonksiyonları
void Telemetry_ClearEStop(TelCtx_t *ctx);
void Telemetry_SetEStopCallback(TelCtx_t *ctx, TelEStopCb_t cb, void *user);
const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx);
void Telemetry_ResetStats(TelCtx_t *ctx);

// Ekrana Yazdırma (Dashboard)
void Telemetry_PrintCompact(const TelData_t *data, TelStatus_t status);
void Telemetry_PrintDashboard(const TelData_t *data, TelStatus_t status, uint8_t estop_active);
void Telemetry_PrintStats(const TelCtx_t *ctx);

#endif /* __TELEMETRY_H */