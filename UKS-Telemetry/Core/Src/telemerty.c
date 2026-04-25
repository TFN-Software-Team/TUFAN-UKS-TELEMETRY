/**
 * @file    telemetry.c
 * @brief   AKS uyumlu ASCII CSV decoder + tek-byte komut encoder.
 *
 *  Parser akisi:
 *  1) ISR (Telemetry_RxBytePush) byte byte gelir.
 *  2) '\r' atilir, '\n' satir sonu kabul edilir; \n gormeden
 *     karakterler line_buf'a yazilir.
 *  3) Satir tamamlandiginda Decode_Line cagrilir:
 *     - Tokenize (virgul ayirici)
 *     - Field sayisi == 15 mi
 *     - Tag == "TEL" mi
 *     - Numerik alanlar ayristirilir (custom parse_int, errno yok)
 *     - Version == TEL_PROTOCOL_VERSION mi
 *     - Sanity range kontrolu
 *     - Sequence izleme (gap / duplicate stats)
 *     - Ping-pong slot'a commit, frame_ready = 1
 *
 *  Parse maliyet: ~70 byte satir + 15 strtol benzeri = 64 MHz Cortex-M
 *  uzerinde mikrosaniyeler. 5 Hz frame hizinda ISR'da yapilmasi guvenli.
 */

#include "telemetry.h"
#include <stdio.h>
#include <string.h>

/* ========== Dahili Yardimcilar ========== */

typedef struct {
    const char *p;
    uint16_t    len;
} Field_t;

/** Buffer'i virgule gore parcalar. Donus: bulunan field sayisi
 *  (max_fields'i asarsa -1). Buffer'a yazmaz, sadece pointer/len verir. */
static int Tokenize(const uint8_t *buf, uint16_t len,
                    Field_t *fields, uint8_t max_fields)
{
    if (max_fields == 0) return -1;
    int n = 0;
    fields[n].p = (const char *)buf;
    uint16_t start = 0;

    for (uint16_t i = 0; i < len; i++)
    {
        if (buf[i] == ',')
        {
            fields[n].len = (uint16_t)(i - start);
            n++;
            if (n >= max_fields) return -1;
            fields[n].p = (const char *)&buf[i + 1];
            start = (uint16_t)(i + 1);
        }
    }
    fields[n].len = (uint16_t)(len - start);
    n++;
    return n;
}

/** Isaretli ondalik tam sayi parser'i. Boslukleri kabul etmez.
 *  Donus: 1=basarili, 0=hata. Tasma 9 haneye kadar guvenli. */
static int Parse_Int(const char *s, uint16_t len,
                     long min_v, long max_v, long *out)
{
    if (len == 0) return 0;
    int neg = 0;
    uint16_t i = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    else if (s[0] == '+')      { i = 1; }
    if (i >= len) return 0;

    long v = 0;
    for (; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 999999999L) return 0;   /* 9 hane tasma korumasi */
    }
    if (neg) v = -v;
    if (v < min_v || v > max_v) return 0;
    *out = v;
    return 1;
}

/** Ping-pong slot'a yazilan TelData_t'yi commit eder ve frame_ready set eder.
 *  Bir frame zaten okunmadiysa overflow_drop sayilir. */
static inline void Commit_Frame(TelCtx_t *ctx)
{
    if (!ctx->frame_ready)
    {
        ctx->read_idx  = ctx->write_idx;
        ctx->write_idx ^= 1;
        ctx->frame_ready = 1;
    }
    else
    {
        /* Ana dongu onceki frame'i okumadi, eski okunabilir frame'i koru */
        ctx->stats.overflow_drop++;
    }
}

/** Sequence numarasinda gap / duplicate / stale tespiti. */
static inline void Track_Sequence(TelCtx_t *ctx, uint32_t seq)
{
    if (ctx->have_last_seq)
    {
        if (seq == ctx->last_sequence || (int32_t)(seq - ctx->last_sequence) < 0)
        {
            ctx->stats.seq_dup_or_stale++;
        }
        else if (seq != ctx->last_sequence + 1)
        {
            ctx->stats.seq_gaps++;
        }
    }
    ctx->last_sequence = seq;
    ctx->have_last_seq = 1;
}

/** Tamamlanan bir satiri ayristir. Hatali satirda stats artar, donus yok. */
static void Decode_Line(TelCtx_t *ctx, const uint8_t *buf, uint16_t len)
{
    Field_t f[TEL_FIELD_COUNT];
    int nf = Tokenize(buf, len, f, TEL_FIELD_COUNT);
    if (nf != (int)TEL_FIELD_COUNT)
    {
        ctx->stats.parse_fail++;
        return;
    }
    ctx->stats.rx_lines++;

    /* 0: tag */
    if (f[0].len != TEL_TAG_LEN ||
        memcmp(f[0].p, TEL_TAG_STR, TEL_TAG_LEN) != 0)
    {
        ctx->stats.bad_tag++;
        return;
    }

    long v_ver, v_seq, v_rpm, v_torq, v_merr, v_mv, v_mt;
    long v_soc, v_bcurr, v_btemp, v_bvolt, v_bcell, v_berr, v_bv;

    if (!Parse_Int(f[1].p,  f[1].len,  0, 255,        &v_ver))   goto pfail;
    if (!Parse_Int(f[2].p,  f[2].len,  0, 2147483647L,&v_seq))   goto pfail;
    if (!Parse_Int(f[3].p,  f[3].len,  0, 65535,      &v_rpm))   goto pfail;
    if (!Parse_Int(f[4].p,  f[4].len, -32768, 32767,  &v_torq))  goto pfail;
    if (!Parse_Int(f[5].p,  f[5].len,  0, 255,        &v_merr))  goto pfail;
    if (!Parse_Int(f[6].p,  f[6].len,  0, 1,          &v_mv))    goto pfail;
    if (!Parse_Int(f[7].p,  f[7].len,  0, 1,          &v_mt))    goto pfail;
    if (!Parse_Int(f[8].p,  f[8].len,  0, 255,        &v_soc))   goto pfail;
    if (!Parse_Int(f[9].p,  f[9].len, -32768, 32767,  &v_bcurr)) goto pfail;
    if (!Parse_Int(f[10].p, f[10].len,-32768, 32767,  &v_btemp)) goto pfail;
    if (!Parse_Int(f[11].p, f[11].len, 0, 65535,      &v_bvolt)) goto pfail;
    if (!Parse_Int(f[12].p, f[12].len, 0, 65535,      &v_bcell)) goto pfail;
    if (!Parse_Int(f[13].p, f[13].len, 0, 255,        &v_berr))  goto pfail;
    if (!Parse_Int(f[14].p, f[14].len, 0, 1,          &v_bv))    goto pfail;

    if ((uint8_t)v_ver != TEL_PROTOCOL_VERSION)
    {
        ctx->stats.bad_version++;
        return;
    }

    /* Sanity */
    if (v_rpm   > TEL_RPM_MAX        ||
        v_soc   > TEL_BMS_SOC_MAX    ||
        v_btemp > TEL_BMS_TEMP_MAX   ||
        v_btemp < TEL_BMS_TEMP_MIN)
    {
        ctx->stats.range_fail++;
        return;
    }

    /* Sequence — gecerli paket sayilmadan once izle ki gap bilgisini
     * sayisal duruma bagimsiz tutalim. */
    Track_Sequence(ctx, (uint32_t)v_seq);

    /* Ping-pong write slot'una yaz */
    TelData_t *d = &ctx->buffers[ctx->write_idx];
    d->protocol_version     = (uint8_t) v_ver;
    d->sequence             = (uint32_t)v_seq;
    d->motor_rpm            = (uint16_t)v_rpm;
    d->motor_torque         = (int16_t) v_torq;
    d->motor_error_flags    = (uint8_t) v_merr;
    d->motor_data_valid     = (uint8_t) v_mv;
    d->motor_timeout_active = (uint8_t) v_mt;
    d->bms_soc              = (uint8_t) v_soc;
    d->bms_current_dA       = (int16_t) v_bcurr;
    d->bms_temp_C           = (int16_t) v_btemp;
    d->bms_pack_voltage_dV  = (uint16_t)v_bvolt;
    d->bms_avg_cell_mV      = (uint16_t)v_bcell;
    d->bms_error_flags      = (uint8_t) v_berr;
    d->bms_data_valid       = (uint8_t) v_bv;

    Commit_Frame(ctx);
    ctx->stats.good_packets++;
    return;

pfail:
    ctx->stats.parse_fail++;
}

/* ========== Encoder ========== */

uint8_t Telemetry_EncodeCommand(uint8_t cmd_byte,
                                uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < 1U) return 0;
    out_buf[0] = cmd_byte;
    return 1U;
}

uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < TEL_ESTOP_BURST_COUNT) return 0;
    for (uint8_t i = 0; i < TEL_ESTOP_BURST_COUNT; i++)
    {
        out_buf[i] = UKS_CMD_EMERGENCY_STOP;
    }
    return TEL_ESTOP_BURST_COUNT;
}

/* ========== Decoder API ========== */

void Telemetry_Init(TelCtx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->line_state = LINE_IDLE;
}

void Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t b, uint32_t now_ms)
{
    if (!ctx) return;

    ctx->stats.rx_bytes++;
    ctx->last_rx_ms = now_ms;

    /* CR atilir. AKS \r\n yolluyor; sadece \n'i satir sonu olarak alalim. */
    if (b == '\r') return;

    if (b == '\n')
    {
        if (ctx->line_len > 0U)
        {
            Decode_Line(ctx, ctx->line_buf, ctx->line_len);
        }
        ctx->line_len   = 0;
        ctx->line_state = LINE_IDLE;
        return;
    }

    /* Yazdirilamayan/control karakterleri sessizce yoksay (debug noise) */
    if (b < 0x20U) return;

    if (ctx->line_len < TEL_LINE_MAX_LEN)
    {
        ctx->line_buf[ctx->line_len++] = b;
        ctx->line_state = LINE_COLLECT;
    }
    else
    {
        /* Tampon dolu — bu satiri at, \n gorunce sifirlanacak */
        ctx->stats.overflow_drop++;
        ctx->line_len   = 0;
        ctx->line_state = LINE_IDLE;
    }
}

uint8_t Telemetry_IsFrameReady(const TelCtx_t *ctx)
{
    return ctx ? ctx->frame_ready : 0;
}

TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out)
{
    if (!ctx || !out)        return TEL_ERR_NULL;
    if (!ctx->frame_ready)   return TEL_NO_DATA;

    /* ISR write_idx'i flip etti, read_idx kararli — kopyala ve flag'i bosalt. */
    *out = ctx->buffers[ctx->read_idx];
    ctx->frame_ready = 0;
    return TEL_VALID;
}

void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return;
    if (ctx->line_state == LINE_IDLE)
    {
        ctx->last_rx_ms = now_ms;
        return;
    }
    if ((now_ms - ctx->last_rx_ms) >= TEL_PARTIAL_TIMEOUT_MS)
    {
        ctx->line_state = LINE_IDLE;
        ctx->line_len   = 0;
        ctx->stats.timeout_drop++;
        ctx->last_rx_ms = now_ms;
    }
}

/* ========== UKS Lokal E-STOP ========== */

uint8_t Telemetry_IsEStopActive(const TelCtx_t *ctx)
{
    return ctx ? ctx->estop_active : 0;
}

void Telemetry_ClearEStop(TelCtx_t *ctx)
{
    if (ctx) ctx->estop_active = 0;
}

void Telemetry_SetEStopActive(TelCtx_t *ctx)
{
    if (!ctx) return;
    if (!ctx->estop_active)
    {
        ctx->estop_active = 1;
        if (ctx->estop_cb) ctx->estop_cb(ctx->estop_cb_user);
    }
}

void Telemetry_SetEStopCallback(TelCtx_t *ctx, TelEStopCb_t cb, void *user)
{
    if (!ctx) return;
    ctx->estop_cb      = cb;
    ctx->estop_cb_user = user;
}

/* ========== Stats ========== */

const TelStats_t *Telemetry_GetStats(const TelCtx_t *ctx)
{
    return ctx ? &ctx->stats : NULL;
}

void Telemetry_ResetStats(TelCtx_t *ctx)
{
    if (ctx) memset(&ctx->stats, 0, sizeof(ctx->stats));
}

/* ========== Ekran ========== */

static void Print_SocBar(uint8_t soc)
{
    const uint8_t bar_w = 20;
    uint8_t filled = (uint8_t)((uint16_t)soc * bar_w / 100U);
    if (filled > bar_w) filled = bar_w;
    printf("[");
    for (uint8_t i = 0; i < bar_w; i++)
        printf("%c", (i < filled) ? '#' : '-');
    printf("]");
}

static const char *Status_Str(TelStatus_t s)
{
    switch (s)
    {
        case TEL_VALID:    return "OK";
        case TEL_NO_DATA:  return "NO_DATA";
        case TEL_ERR_NULL: return "NULL";
        default:           return "UNK";
    }
}

void Telemetry_PrintDashboard(const TelData_t *d, TelStatus_t status,
                              uint8_t estop_active)
{
    printf("\r\n");
    printf("  +============================================+\r\n");
    printf("  |        UKS YER ISTASYONU TELEMETRI         |\r\n");
    printf("  +============================================+\r\n");

    if (estop_active)
    {
        printf("  |  *** !!! ACIL DURDURMA AKTIF !!! ***       |\r\n");
        printf("  |  Motor devre disi - manuel reset gerek    |\r\n");
        printf("  |--------------------------------------------|\r\n");
    }

    if (status == TEL_NO_DATA || !d)
    {
        printf("  |  ** AKS'ten veri bekleniyor...            |\r\n");
        printf("  +============================================+\r\n\r\n");
        return;
    }

    printf("  |  Durum: %-7s  Seq: %-8lu Ver: %u       |\r\n",
           Status_Str(status), (unsigned long)d->sequence,
           (unsigned)d->protocol_version);
    printf("  |--- Motor ----------------------------------|\r\n");
    printf("  |   RPM    : %5u    Torque : %6d         |\r\n",
           estop_active ? 0U : d->motor_rpm,
           (int)d->motor_torque);
    printf("  |   Errs   : 0x%02X     Valid  : %u  Tout: %u  |\r\n",
           (unsigned)d->motor_error_flags,
           (unsigned)d->motor_data_valid,
           (unsigned)d->motor_timeout_active);
    printf("  |--- BMS ------------------------------------|\r\n");
    printf("  |   SoC    : %3u%%  ", (unsigned)d->bms_soc);
    Print_SocBar(d->bms_soc);
    printf(" |\r\n");

    /* Curr deci-A -> A.dA, Volt deci-V -> V.dV */
    int   ca  = d->bms_current_dA      / 10;
    int   cd  = d->bms_current_dA % 10; if (cd < 0) cd = -cd;
    unsigned va = (unsigned)(d->bms_pack_voltage_dV / 10U);
    unsigned vd = (unsigned)(d->bms_pack_voltage_dV % 10U);
    printf("  |   Curr   : %4d.%d A   Pack : %3u.%u V      |\r\n",
           ca, cd, va, vd);

    printf("  |   Temp   : %4d C", (int)d->bms_temp_C);
    if      (d->bms_temp_C > 60) printf("    !! YUKSEK !!");
    else if (d->bms_temp_C > 45) printf("    !  UYARI  !");
    else                         printf("                ");
    printf("    |\r\n");

    printf("  |   Cell   : %4u mV    Errs : 0x%02X  V:%u  |\r\n",
           (unsigned)d->bms_avg_cell_mV,
           (unsigned)d->bms_error_flags,
           (unsigned)d->bms_data_valid);
    printf("  +============================================+\r\n\r\n");
}

void Telemetry_PrintStats(const TelCtx_t *ctx)
{
    if (!ctx) return;
    const TelStats_t *s = &ctx->stats;
    printf("\r\n  --- Istatistikler ---\r\n");
    printf("  RX byte         : %lu\r\n", (unsigned long)s->rx_bytes);
    printf("  RX satir        : %lu\r\n", (unsigned long)s->rx_lines);
    printf("  Parse hata      : %lu\r\n", (unsigned long)s->parse_fail);
    printf("  Tag hata        : %lu\r\n", (unsigned long)s->bad_tag);
    printf("  Version hata    : %lu\r\n", (unsigned long)s->bad_version);
    printf("  Range hata      : %lu\r\n", (unsigned long)s->range_fail);
    printf("  Timeout/overflow: %lu / %lu\r\n",
           (unsigned long)s->timeout_drop,
           (unsigned long)s->overflow_drop);
    printf("  Gecerli pkt     : %lu\r\n", (unsigned long)s->good_packets);
    printf("  Seq gap         : %lu\r\n", (unsigned long)s->seq_gaps);
    printf("  Seq dup/stale   : %lu\r\n", (unsigned long)s->seq_dup_or_stale);
    printf("  E-STOP TX (UKS) : %lu\r\n", (unsigned long)s->estop_tx_count);
    printf("  E-STOP aktif    : %s\r\n", ctx->estop_active ? "EVET" : "hayir");
    printf("  ---------------------\r\n\r\n");
}