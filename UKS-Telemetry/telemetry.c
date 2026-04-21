/**
 * @file    telemetry.c
 * @brief   Byte-stuffing destekli telemetri modulu implementasyonu.
 */

#include "telemetry.h"
#include <stdio.h>
#include <string.h>

/* ========== Dahili Yardimcilar ========== */

/**
 * @brief  Tek bir byte'i stuff eder (encode).
 *         0xAA -> 0xAB 0x01,  0xAB -> 0xAB 0x02,  diger -> oldugu gibi.
 * @return Yazilan byte sayisi (1 veya 2)
 */
static uint8_t Stuff_Byte(uint8_t in, uint8_t *out)
{
    if (in == TEL_HEADER_BYTE)
    {
        out[0] = TEL_ESC_BYTE;
        out[1] = TEL_ESC_HEADER;
        return 2;
    }
    if (in == TEL_ESC_BYTE)
    {
        out[0] = TEL_ESC_BYTE;
        out[1] = TEL_ESC_ESC;
        return 2;
    }
    out[0] = in;
    return 1;
}

static uint8_t Range_Check(const TelData_t *d)
{
    if (d->soc > TEL_SOC_MAX)               return 0;
    if (d->battery_temp > TEL_BAT_TEMP_MAX)  return 0;
    if (d->motor_rpm > TEL_RPM_MAX)          return 0;
    return 1;
}

/* ========== Gonderici (Encoder) ========== */

uint8_t Telemetry_Encode(const TelData_t *data, uint8_t *out_buf)
{
    /* Ham paketi olustur */
    uint8_t raw[TEL_RAW_PACKET_LEN];
    raw[TEL_IDX_HEADER]   = TEL_HEADER_BYTE;
    raw[TEL_IDX_SPEED]    = data->speed;
    raw[TEL_IDX_SOC]      = data->soc;
    raw[TEL_IDX_BAT_TEMP] = data->battery_temp;
    raw[TEL_IDX_RPM_HI]   = (uint8_t)(data->motor_rpm >> 8);
    raw[TEL_IDX_RPM_LO]   = (uint8_t)(data->motor_rpm & 0xFF);

    /* Checksum: XOR of bytes [0..5] */
    uint8_t chk = 0;
    for (uint8_t i = 0; i < TEL_PAYLOAD_LEN; i++)
    {
        chk ^= raw[i];
    }
    raw[TEL_IDX_CHK] = chk;

    /* Header'i oldugu gibi yaz */
    uint8_t pos = 0;
    out_buf[pos++] = TEL_HEADER_BYTE;

    /* Payload byte'larini (1..6) stuff ederek yaz */
    for (uint8_t i = 1; i < TEL_RAW_PACKET_LEN; i++)
    {
        pos += Stuff_Byte(raw[i], &out_buf[pos]);
    }

    return pos;  /* toplam yazilan byte */
}

/* ========== Alici (Decoder + Parser) ========== */

void Telemetry_Init(TelCtx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->decoder.state = FRAME_IDLE;
}

void Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t rx_byte)
{
    ctx->stats.rx_bytes++;
    FrameDecoder_t *dec = &ctx->decoder;

    switch (dec->state)
    {
    case FRAME_IDLE:
        if (rx_byte == TEL_HEADER_BYTE)
        {
            dec->idx   = 0;
            dec->state = FRAME_PAYLOAD;
        }
        /* Header degilse yoksay */
        break;

    case FRAME_PAYLOAD:
        if (rx_byte == TEL_HEADER_BYTE)
        {
            /*
             * Yeni header geldi — onceki frame eksik kaldi.
             * Mevcut frame'i sil, yeni frame basla.
             */
            dec->idx   = 0;
            /* state zaten FRAME_PAYLOAD */
            break;
        }
        if (rx_byte == TEL_ESC_BYTE)
        {
            dec->state = FRAME_ESC;
            break;
        }
        /* Normal byte */
        if (dec->idx < TEL_PAYLOAD_LEN)
        {
            dec->payload[dec->idx++] = rx_byte;
        }
        /* Payload tamam mi? */
        if (dec->idx >= TEL_PAYLOAD_LEN)
        {
            /* Frame'i frame_buf'a kopyala */
            if (!ctx->frame_ready)   /* onceki okunmadiysa ustune yazma */
            {
                memcpy(ctx->frame_buf, dec->payload, TEL_PAYLOAD_LEN);
                ctx->frame_ready = 1;
            }
            else
            {
                ctx->stats.rx_drop++;
            }
            dec->state = FRAME_IDLE;
        }
        break;

    case FRAME_ESC:
        if (rx_byte == TEL_ESC_HEADER)
        {
            /* 0xAB 0x01 -> gercek 0xAA */
            if (dec->idx < TEL_PAYLOAD_LEN)
                dec->payload[dec->idx++] = TEL_HEADER_BYTE;
        }
        else if (rx_byte == TEL_ESC_ESC)
        {
            /* 0xAB 0x02 -> gercek 0xAB */
            if (dec->idx < TEL_PAYLOAD_LEN)
                dec->payload[dec->idx++] = TEL_ESC_BYTE;
        }
        else
        {
            /* Gecersiz escape dizisi — frame'i at */
            ctx->stats.stuff_err++;
            dec->state = FRAME_IDLE;
            break;
        }

        dec->state = FRAME_PAYLOAD;

        /* Payload tamam mi? */
        if (dec->idx >= TEL_PAYLOAD_LEN)
        {
            if (!ctx->frame_ready)
            {
                memcpy(ctx->frame_buf, dec->payload, TEL_PAYLOAD_LEN);
                ctx->frame_ready = 1;
            }
            else
            {
                ctx->stats.rx_drop++;
            }
            dec->state = FRAME_IDLE;
        }
        break;
    }
}

uint8_t Telemetry_IsFrameReady(const TelCtx_t *ctx)
{
    return ctx->frame_ready;
}

TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out)
{
    if (!ctx->frame_ready)
    {
        return TEL_NO_DATA;
    }

    const uint8_t *pl = ctx->frame_buf;   /* payload: index 0..5 */

    /*
     * Checksum dogrulama:
     * Ham pakette chk = XOR(raw[0..5]),  raw[0] = 0xAA.
     * payload[0..4] = raw[1..5],  payload[5] = raw[6] = chk.
     *
     * Yeniden hesapla: XOR(0xAA, pl[0], pl[1], pl[2], pl[3], pl[4])
     * ve payload[5] ile karsilastir.
     */
    uint8_t chk = TEL_HEADER_BYTE;
    for (uint8_t i = 0; i < TEL_PAYLOAD_LEN - 1; i++)
    {
        chk ^= pl[i];
    }

    /* frame'i tuketildi olarak isaretle */
    ctx->frame_ready = 0;

    if (chk != pl[TEL_PAYLOAD_LEN - 1])
    {
        ctx->stats.chk_fail++;
        return TEL_CHK_FAIL;
    }

    /* payload[0]=speed, [1]=soc, [2]=bat_temp, [3]=rpm_hi, [4]=rpm_lo */
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

void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms)
{
    if (ctx->decoder.state == FRAME_IDLE)
    {
        ctx->last_rx_ms = now_ms;
        return;
    }

    if ((now_ms - ctx->last_rx_ms) >= TEL_PARTIAL_TIMEOUT_MS)
    {
        /* Eksik frame — decoder'i sifirla */
        ctx->decoder.state = FRAME_IDLE;
        ctx->decoder.idx   = 0;
        ctx->stats.timeout_drop++;
        ctx->last_rx_ms = now_ms;
    }
}

/* ========== Istatistik ========== */

const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx)
{
    return &ctx->stats;
}

void Telemetry_ResetStats(TelCtx_t *ctx)
{
    memset(&ctx->stats, 0, sizeof(ctx->stats));
}

/* ========== Yer Istasyonu Ekrani ========== */

/**
 * @brief  SoC degerine gore basit ASCII bar olusturur.
 */
static void Print_SocBar(uint8_t soc)
{
    const uint8_t bar_width = 20;
    uint8_t filled = (uint8_t)((uint16_t)soc * bar_width / 100);

    printf("[");
    for (uint8_t i = 0; i < bar_width; i++)
    {
        printf("%c", (i < filled) ? '#' : '-');
    }
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
        default:               return "UNKNOWN";
    }
}

void Telemetry_PrintCompact(const TelData_t *data, TelStatus_t status)
{
    if (status == TEL_NO_DATA)
    {
        printf("[TEL] Veri yok\n");
        return;
    }

    printf("[TEL][%s] SPD:%3u km/h | SOC:%3u%% | TEMP:%3u C | RPM:%5u\n",
           Status_Str(status),
           data->speed,
           data->soc,
           data->battery_temp,
           data->motor_rpm);
}

void Telemetry_PrintDashboard(const TelData_t *data, TelStatus_t status)
{
    printf("\n");
    printf("  +======================================+\n");
    printf("  |        YER ISTASYONU TELEMETRI       |\n");
    printf("  +======================================+\n");

    if (status == TEL_NO_DATA)
    {
        printf("  |  ** Veri bekleniyor...             |\n");
        printf("  +======================================+\n\n");
        return;
    }

    printf("  |  Durum   : %-10s                |\n", Status_Str(status));
    printf("  |--------------------------------------|\n");
    printf("  |  Hiz     : %3u km/h                  |\n", data->speed);
    printf("  |  SoC     : %3u%%  ", data->soc);
    Print_SocBar(data->soc);
    printf("  |\n");
    printf("  |  Bat.Sic : %3u C", data->battery_temp);

    if (data->battery_temp > 60)
        printf("   !! YUKSEK !!");
    else if (data->battery_temp > 45)
        printf("   !  UYARI  !");
    else
        printf("              ");
    printf("  |\n");

    printf("  |  Motor   : %5u RPM                 |\n", data->motor_rpm);
    printf("  +======================================+\n\n");
}

void Telemetry_PrintStats(const TelCtx_t *ctx)
{
    const TelStats_t *s = &ctx->stats;

    printf("\n  --- Istatistikler ---\n");
    printf("  RX byte     : %u\n", (unsigned)s->rx_bytes);
    printf("  Drop (dolu) : %u\n", (unsigned)s->rx_drop);
    printf("  CHK hata    : %u\n", (unsigned)s->chk_fail);
    printf("  Range hata  : %u\n", (unsigned)s->range_fail);
    printf("  Timeout     : %u\n", (unsigned)s->timeout_drop);
    printf("  Stuff hata  : %u\n", (unsigned)s->stuff_err);
    printf("  Gecerli pkt : %u\n", (unsigned)s->good_packets);
    printf("  -----------------------\n\n");
}
