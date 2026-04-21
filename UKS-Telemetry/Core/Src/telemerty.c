/**
 * @file    telemetry.c
 * @brief   Byte-stuffing destekli, cift-tamponlu telemetri + komut modulu.
 *
 *  Mimari ozeti:
 *  - Telemetri frame'leri: ping-pong cift tampon (buffers[2]). ISR bir
 *    tamponu doldururken ana dongu digerini okur — kopyalama yok.
 *  - Komut frame'leri    : tek tampon + snapshot (ParseCommand memcpy ile
 *    yerel kopya alir). Komutlar nadir oldugu icin ping-pong gereksiz.
 *  - E-STOP              : ISR icinde cmd frame commit edilirken checksum
 *    dogrulanir ve estop_active aninda latchlenir. Ana dongu yavas olsa
 *    bile acil durdurma anlik tetiklenir.
 */

#include "telemetry.h"
#include <stdio.h>
#include <string.h>

/* ========== Dahili Yardimcilar ========== */

/** Byte stuffing: 0xAA/0xAB/0xCC -> escape dizisi, diger -> oldugu gibi. */
static inline uint8_t Stuff_Byte(uint8_t in, uint8_t *out)
{
    if (in == TEL_HEADER_BYTE)     { out[0] = TEL_ESC_BYTE; out[1] = TEL_ESC_HEADER; return 2; }
    if (in == TEL_ESC_BYTE)        { out[0] = TEL_ESC_BYTE; out[1] = TEL_ESC_ESC;    return 2; }
    if (in == TEL_CMD_HEADER_BYTE) { out[0] = TEL_ESC_BYTE; out[1] = TEL_ESC_CMD;    return 2; }
    out[0] = in;
    return 1;
}

static uint8_t Range_Check(const TelData_t *d)
{
    if (d->soc > TEL_SOC_MAX)                return 0;
    if (d->battery_temp > TEL_BAT_TEMP_MAX)  return 0;
    if (d->motor_rpm > TEL_RPM_MAX)          return 0;
    return 1;
}

/* ========== Gonderici (Encoder) ========== */

uint8_t Telemetry_Encode(const TelData_t *data, uint8_t *out_buf, size_t max_len)
{
    if (!data || !out_buf || max_len < (TEL_RAW_PACKET_LEN * 2)) return 0;

    uint8_t raw[TEL_RAW_PACKET_LEN];
    raw[TEL_IDX_HEADER]   = TEL_HEADER_BYTE;
    raw[TEL_IDX_SPEED]    = data->speed;
    raw[TEL_IDX_SOC]      = data->soc;
    raw[TEL_IDX_BAT_TEMP] = data->battery_temp;
    raw[TEL_IDX_RPM_HI]   = (uint8_t)(data->motor_rpm >> 8);
    raw[TEL_IDX_RPM_LO]   = (uint8_t)(data->motor_rpm & 0xFF);
    raw[TEL_IDX_CHK]      = raw[0] ^ raw[1] ^ raw[2] ^ raw[3] ^ raw[4] ^ raw[5];

    uint8_t pos = 0;
    out_buf[pos++] = TEL_HEADER_BYTE;
    for (uint8_t i = 1; i < TEL_RAW_PACKET_LEN; i++)
    {
        pos += Stuff_Byte(raw[i], &out_buf[pos]);
    }
    return pos;
}

uint8_t Telemetry_EncodeCommand(TelCmd_t cmd, uint8_t param,
                                uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < TEL_CMD_STUFFED_MAX_LEN) return 0;

    uint8_t raw[TEL_CMD_RAW_LEN];
    raw[0] = TEL_CMD_HEADER_BYTE;
    raw[1] = (uint8_t)cmd;
    raw[2] = param;
    raw[3] = raw[0] ^ raw[1] ^ raw[2];

    uint8_t pos = 0;
    out_buf[pos++] = TEL_CMD_HEADER_BYTE;
    for (uint8_t i = 1; i < TEL_CMD_RAW_LEN; i++)
    {
        pos += Stuff_Byte(raw[i], &out_buf[pos]);
    }
    return pos;
}

uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < TEL_ESTOP_BURST_MAX_LEN) return 0;

    uint8_t total = 0;
    for (uint8_t i = 0; i < TEL_ESTOP_BURST_COUNT; i++)
    {
        total += Telemetry_EncodeCommand(TEL_CMD_ESTOP, 0,
                                         &out_buf[total], max_len - total);
    }
    return total;
}

/* ========== Decoder Yardimcilari ========== */

/** Yeni bir frame baslatir (header goruldugunde). */
static inline void Decoder_StartFrame(FrameDecoder_t *dec, uint8_t type)
{
    dec->type         = type;
    dec->idx          = 0;
    dec->expected_len = (type == FRAME_TYPE_TELEMETRY)
                            ? TEL_PAYLOAD_LEN
                            : TEL_CMD_PAYLOAD_LEN;
    dec->state        = FRAME_PAYLOAD;
}

/** Decoder icindeki byte'i uygun tampona yazar (tip + indekse gore). */
static inline void Decoder_WriteByte(TelCtx_t *ctx, uint8_t b)
{
    FrameDecoder_t *dec = &ctx->decoder;
    if (dec->idx >= dec->expected_len) return;

    if (dec->type == FRAME_TYPE_TELEMETRY)
        ctx->buffers[ctx->write_idx][dec->idx++] = b;
    else
        ctx->cmd_buf[dec->idx++] = b;
}

/**
 * Komut frame tamamlandi — ISR icinde checksum'u dogrula ve E-STOP'i
 * aninda latchle. Diger komutlar (PING vb.) ana dongude parse edilir.
 */
static inline void Decoder_CommitCommand(TelCtx_t *ctx)
{
    const uint8_t *pl = ctx->cmd_buf;
    uint8_t chk = (uint8_t)(TEL_CMD_HEADER_BYTE ^ pl[0] ^ pl[1]);

    if (chk != pl[2])
    {
        ctx->stats.cmd_chk_fail++;
        return;
    }

    /* Fast path: E-STOP'i ISR icinde latchle */
    if (pl[0] == (uint8_t)TEL_CMD_ESTOP)
    {
        ctx->stats.estop_rx_count++;
        if (!ctx->estop_active)
        {
            ctx->estop_active = 1;
            if (ctx->estop_cb) ctx->estop_cb(ctx->estop_cb_user);
        }
    }

    /* Ana dongude ParseCommand ile okunacak */
    if (!ctx->cmd_ready)
        ctx->cmd_ready = 1;
    else
        ctx->stats.rx_drop++;
}

/** Telemetri frame tamamlandi — ping-pong cevir, ana dongu okusun. */
static inline void Decoder_CommitTelemetry(TelCtx_t *ctx)
{
    if (!ctx->frame_ready)
    {
        ctx->read_idx  = ctx->write_idx;
        ctx->write_idx ^= 1;
        ctx->frame_ready = 1;
    }
    else
    {
        ctx->stats.rx_drop++;
    }
}

/* ========== API ========== */

void Telemetry_Init(TelCtx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->decoder.state = FRAME_IDLE;
}

void Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t rx_byte, uint32_t now_ms)
{
    if (!ctx) return;

    ctx->stats.rx_bytes++;
    ctx->last_rx_ms = now_ms;

    FrameDecoder_t *dec = &ctx->decoder;

    switch (dec->state)
    {
    case FRAME_IDLE:
        if      (rx_byte == TEL_HEADER_BYTE)     Decoder_StartFrame(dec, FRAME_TYPE_TELEMETRY);
        else if (rx_byte == TEL_CMD_HEADER_BYTE) Decoder_StartFrame(dec, FRAME_TYPE_COMMAND);
        /* Diger byte'lari yoksay */
        break;

    case FRAME_PAYLOAD:
        /* Yeni header mid-frame — mevcut frame'i iptal et, yenisini baslat.
         * 0xCC (komut) mid-frame geldiginde E-STOP ONCELIGI igin yeni frame
         * olarak ele alinir — telemetri frame'i iptal edilir. */
        if (rx_byte == TEL_HEADER_BYTE)
        {
            Decoder_StartFrame(dec, FRAME_TYPE_TELEMETRY);
            break;
        }
        if (rx_byte == TEL_CMD_HEADER_BYTE)
        {
            Decoder_StartFrame(dec, FRAME_TYPE_COMMAND);
            break;
        }
        if (rx_byte == TEL_ESC_BYTE)
        {
            dec->state = FRAME_ESC;
            break;
        }

        Decoder_WriteByte(ctx, rx_byte);

        if (dec->idx >= dec->expected_len)
        {
            if (dec->type == FRAME_TYPE_TELEMETRY) Decoder_CommitTelemetry(ctx);
            else                                   Decoder_CommitCommand(ctx);
            dec->state = FRAME_IDLE;
        }
        break;

    case FRAME_ESC:
    {
        uint8_t decoded;
        if      (rx_byte == TEL_ESC_HEADER) decoded = TEL_HEADER_BYTE;
        else if (rx_byte == TEL_ESC_ESC)    decoded = TEL_ESC_BYTE;
        else if (rx_byte == TEL_ESC_CMD)    decoded = TEL_CMD_HEADER_BYTE;
        else if (rx_byte == TEL_HEADER_BYTE)
        {
            /* Kurtarma: 0xAB sonrasi ham 0xAA -> hatali escape, yeni
             * telemetri frame olarak restart */
            ctx->stats.stuff_err++;
            Decoder_StartFrame(dec, FRAME_TYPE_TELEMETRY);
            break;
        }
        else if (rx_byte == TEL_CMD_HEADER_BYTE)
        {
            /* Kurtarma: 0xAB sonrasi ham 0xCC -> yeni komut frame */
            ctx->stats.stuff_err++;
            Decoder_StartFrame(dec, FRAME_TYPE_COMMAND);
            break;
        }
        else
        {
            ctx->stats.stuff_err++;
            dec->state = FRAME_IDLE;
            break;
        }

        Decoder_WriteByte(ctx, decoded);
        dec->state = FRAME_PAYLOAD;

        if (dec->idx >= dec->expected_len)
        {
            if (dec->type == FRAME_TYPE_TELEMETRY) Decoder_CommitTelemetry(ctx);
            else                                   Decoder_CommitCommand(ctx);
            dec->state = FRAME_IDLE;
        }
        break;
    }
    }
}

uint8_t Telemetry_IsFrameReady(const TelCtx_t *ctx)
{
    return ctx ? ctx->frame_ready : 0;
}

uint8_t Telemetry_IsCommandReady(const TelCtx_t *ctx)
{
    return ctx ? ctx->cmd_ready : 0;
}

TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out)
{
    if (!ctx || !out) return TEL_ERR_NULL;
    if (!ctx->frame_ready) return TEL_NO_DATA;

    const uint8_t *pl = ctx->buffers[ctx->read_idx];
    ctx->frame_ready = 0;  /* ISR yeni frame kabul edebilir */

    uint8_t chk = TEL_HEADER_BYTE ^ pl[0] ^ pl[1] ^ pl[2] ^ pl[3] ^ pl[4];
    if (chk != pl[5])
    {
        ctx->stats.chk_fail++;
        return TEL_CHK_FAIL;
    }

    out->speed        = pl[0];
    out->soc          = pl[1];
    out->battery_temp = pl[2];
    out->motor_rpm    = ((uint16_t)pl[3] << 8) | pl[4];

    if (!Range_Check(out))
    {
        ctx->stats.range_fail++;
        return TEL_OUT_OF_RANGE;
    }

    ctx->stats.good_packets++;
    return TEL_VALID;
}

TelStatus_t Telemetry_ParseCommand(TelCtx_t *ctx, TelCmd_t *cmd_out, uint8_t *param_out)
{
    if (!ctx || !cmd_out || !param_out) return TEL_ERR_NULL;
    if (!ctx->cmd_ready) return TEL_NO_DATA;

    /* ISR cmd_buf'i bir sonraki komut icin yeniden yazmaya baslayabilir
     * — once yerel snapshot al, sonra flag'i bosalt. */
    uint8_t pl[TEL_CMD_PAYLOAD_LEN];
    memcpy(pl, ctx->cmd_buf, sizeof(pl));
    ctx->cmd_ready = 0;

    /* ISR zaten checksum'u dogruladi ve (varsa) E-STOP'u latchledi.
     * Burada sadece komut tiplemesi yapiyoruz. */
    *cmd_out   = (TelCmd_t)pl[0];
    *param_out = pl[1];

    switch (pl[0])
    {
    case TEL_CMD_ESTOP:
    case TEL_CMD_ESTOP_CLEAR:
    case TEL_CMD_PING:
        ctx->stats.good_commands++;
        return TEL_VALID;
    default:
        return TEL_UNKNOWN_CMD;
    }
}

void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return;

    if (ctx->decoder.state == FRAME_IDLE)
    {
        ctx->last_rx_ms = now_ms;
        return;
    }

    if ((now_ms - ctx->last_rx_ms) >= TEL_PARTIAL_TIMEOUT_MS)
    {
        ctx->decoder.state = FRAME_IDLE;
        ctx->decoder.idx   = 0;
        ctx->stats.timeout_drop++;
        ctx->last_rx_ms = now_ms;
    }
}

/* ========== E-STOP Kontrolu ========== */

uint8_t Telemetry_IsEStopActive(const TelCtx_t *ctx)
{
    return ctx ? ctx->estop_active : 0;
}

void Telemetry_ClearEStop(TelCtx_t *ctx)
{
    if (ctx) ctx->estop_active = 0;
}

void Telemetry_SetEStopCallback(TelCtx_t *ctx, TelEStopCb_t cb, void *user)
{
    if (!ctx) return;
    ctx->estop_cb      = cb;
    ctx->estop_cb_user = user;
}

/* ========== Istatistik ========== */

const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx)
{
    return ctx ? &ctx->stats : NULL;
}

void Telemetry_ResetStats(TelCtx_t *ctx)
{
    if (ctx) memset(&ctx->stats, 0, sizeof(ctx->stats));
}

/* ========== Yer Istasyonu Ekrani ========== */

static void Print_SocBar(uint8_t soc)
{
    const uint8_t bar_width = 20;
    uint8_t filled = (uint8_t)((uint16_t)soc * bar_width / 100);
    printf("[");
    for (uint8_t i = 0; i < bar_width; i++)
        printf("%c", (i < filled) ? '#' : '-');
    printf("]");
}

static const char *Status_Str(TelStatus_t s)
{
    switch (s)
    {
        case TEL_VALID:        return "OK";
        case TEL_NO_DATA:      return "NO_DATA";
        case TEL_CHK_FAIL:     return "CHK_FAIL";
        case TEL_OUT_OF_RANGE: return "RANGE_ERR";
        case TEL_ERR_NULL:     return "NULL_PTR";
        case TEL_UNKNOWN_CMD:  return "UNK_CMD";
        default:               return "UNKNOWN";
    }
}

void Telemetry_PrintCompact(const TelData_t *data, TelStatus_t status)
{
    if (status == TEL_NO_DATA) { printf("[TEL] Veri yok\n"); return; }
    printf("[TEL][%s] SPD:%3u km/h | SOC:%3u%% | TEMP:%3u C | RPM:%5u\n",
           Status_Str(status),
           data->speed, data->soc, data->battery_temp, data->motor_rpm);
}

void Telemetry_PrintDashboard(const TelData_t *data, TelStatus_t status,
                              uint8_t estop_active)
{
    printf("\n");
    printf("  +======================================+\n");
    printf("  |        YER ISTASYONU TELEMETRI       |\n");
    printf("  +======================================+\n");

    if (estop_active)
    {
        printf("  |  *** !!! ACIL DURDURMA AKTIF !!! *** |\n");
        printf("  |   Motor devre disi — manuel reset    |\n");
        printf("  |--------------------------------------|\n");
    }

    if (status == TEL_NO_DATA)
    {
        printf("  |  ** Veri bekleniyor...               |\n");
        printf("  +======================================+\n\n");
        return;
    }

    printf("  |  Durum   : %-10s                |\n", Status_Str(status));
    printf("  |--------------------------------------|\n");
    printf("  |  Hiz     : %3u km/h                  |\n",
           estop_active ? 0 : data->speed);
    printf("  |  SoC     : %3u%%  ", data->soc);
    Print_SocBar(data->soc);
    printf("  |\n");
    printf("  |  Bat.Sic : %3u C", data->battery_temp);

    if      (data->battery_temp > 60) printf("   !! YUKSEK !!");
    else if (data->battery_temp > 45) printf("   !  UYARI  !");
    else                              printf("              ");
    printf("  |\n");

    printf("  |  Motor   : %5u RPM                 |\n",
           estop_active ? 0 : data->motor_rpm);
    printf("  +======================================+\n\n");
}

void Telemetry_PrintStats(const TelCtx_t *ctx)
{
    if (!ctx) return;
    const TelStats_t *s = &ctx->stats;

    printf("\n  --- Istatistikler ---\n");
    printf("  RX byte        : %u\n", (unsigned)s->rx_bytes);
    printf("  Drop (dolu)    : %u\n", (unsigned)s->rx_drop);
    printf("  CHK hata       : %u\n", (unsigned)s->chk_fail);
    printf("  Range hata     : %u\n", (unsigned)s->range_fail);
    printf("  Timeout        : %u\n", (unsigned)s->timeout_drop);
    printf("  Stuff hata     : %u\n", (unsigned)s->stuff_err);
    printf("  Gecerli pkt    : %u\n", (unsigned)s->good_packets);
    printf("  Gecerli komut  : %u\n", (unsigned)s->good_commands);
    printf("  Komut CHK hata : %u\n", (unsigned)s->cmd_chk_fail);
    printf("  E-STOP alindi  : %u\n", (unsigned)s->estop_rx_count);
    printf("  E-STOP aktif   : %s\n", ctx->estop_active ? "EVET" : "hayir");
    printf("  -----------------------\n\n");
}
