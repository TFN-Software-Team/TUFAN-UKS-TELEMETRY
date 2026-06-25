/**
 * @file    telemetry.c
 * @brief   AKS uyumlu ASCII CSV decoder + tek-byte komut encoder.
 *
 *  Duzeltmeler (v2):
 *  1) Commit_Frame race condition giderildi: read_idx/write_idx/frame_ready
 *     atamalari critical section (PRIMASK) icine alindi.
 *  2) Telemetry_Parse icindeki frame_ready=0 atamasi da critical section
 *     icine alindi; ISR ile cakisma engellendi.
 *  3) Telemetry_Tick: LINE_IDLE durumunda last_rx_ms sifirlanmiyordu,
 *     bu timeout saatini yanlis sifirliyor ve LINE_COLLECT'e girer
 *     girmez timeout'u uzatiyordu. IDLE branch'i kaldirildi.
 *
 *  Duzeltmeler (v3 — ISR yuku azaltildi):
 *  4) ISR'da parse YAPILMIYOR. Telemetry_RxBytePush artik sadece gelen
 *     byte'i dairesel tampona (ring buffer) yazar. Eski byte-isleme
 *     mantigi (satir birlestirme + Decode_Line) Telemetry_Process'e
 *     tasindi ve ana donguden cagriliyor.
 *
 *     Gerekce: HSI 8 MHz'de (klon-guvenli saat) Decode_Line en kotu
 *     senaryoda ~1 ms surebiliyor; 9600 baud byte penceresi (~1.04 ms)
 *     ile cakisip Overrun (ORE) riski doguruyordu. Artik ISR sabit ve
 *     mikrosaniye seviyesinde; parse suresi kritik degil cunku ring
 *     buffer arka planda dolmaya devam eder.
 *
 *     NOT: Decode_Line / Parse_Int / Tokenize / Commit_Frame / Track_Sequence
 *     mantigi DEGISMEDI — yalnizca cagrildiklari baglam (ISR → main) degisti.
 *     Commit_Frame'deki PRIMASK bloku artik teknik olarak gereksiz (Process
 *     ve ana dongu ayni baglamda) ama zararsiz oldugu icin korundu.
 */

#include "telemetry.h"
#include "stm32f1xx.h"   /* __disable_irq / __enable_irq icin */
#include <stdio.h>
#include <string.h>

/* ========== Dahili Yardimcilar ========== */

typedef struct {
    const char *p;
    uint16_t    len;
} Field_t;

/**
 * Buffer'i virgule gore parcalar.
 * Donus: bulunan field sayisi; max_fields'i asarsa -1.
 */
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

/**
 * Isaretli ondalik tam sayi parser'i. Bosluk kabul etmez.
 * Donus: 1=basarili, 0=hata. 9 haneye kadar tasma-guvenli.
 */
static int Parse_Int(const char *s, uint16_t len,
                     long min_v, long max_v, long *out)
{
    if (len == 0) return 0;
    int neg = 0;
    uint16_t i = 0;
    if      (s[0] == '-') { neg = 1; i = 1; }
    else if (s[0] == '+') { i = 1; }
    if (i >= len) return 0;

    long v = 0;
    for (; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9') return 0;
        /* BUG #9 DUZELTME (v2): Son digit de kontrol edilmeli.
         * v==214748364 iken digit>=8 gelirse v*10+digit = 2147483648
         * -> signed long overflow -> Undefined Behavior (-O2'de UB
         * derleyici tarafindan optimize edilebilir). */
        if (v > 214748364L ||
            (v == 214748364L && (s[i] - '0') > 7)) return 0;
        v = v * 10 + (s[i] - '0');
    }
    if (neg) v = -v;
    if (v < min_v || v > max_v) return 0;
    *out = v;
    return 1;
}

/**
 * Commit_Frame — cift tampon (ping-pong) commit.
 *
 * v3 NOT: Artik hem Commit_Frame hem okuma ana baglamda (main context)
 * oldugundan PRIMASK bloku teknik olarak gereksiz. Ancak ileride yeniden
 * ISR'a tasinma ihtimaline karsi ve zararsiz oldugu icin korundu.
 */
static inline void Commit_Frame(TelCtx_t *ctx)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (!ctx->frame_ready)
    {
        ctx->read_idx    = ctx->write_idx;
        ctx->write_idx  ^= 1U;
        ctx->frame_ready = 1U;
    }
    else
    {
        ctx->stats.overflow_drop++;
    }

    __set_PRIMASK(primask);
}

/** Sequence numarasinda gap / duplicate / stale tespiti. */
static inline void Track_Sequence(TelCtx_t *ctx, uint32_t seq)
{
    if (ctx->have_last_seq)
    {
        if (seq == ctx->last_sequence ||
            (int32_t)(seq - ctx->last_sequence) < 0)
        {
            ctx->stats.seq_dup_or_stale++;
        }
        else if (seq != ctx->last_sequence + 1U)
        {
            ctx->stats.seq_gaps++;
        }
    }
    ctx->last_sequence = seq;
    ctx->have_last_seq = 1U;
}

/** Tamamlanan bir satiri ayristir. Hatali satirda stats artar. */
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

    /* Alan 0: tag kontrolu */
    if (f[0].len != TEL_TAG_LEN ||
        memcmp(f[0].p, TEL_TAG_STR, TEL_TAG_LEN) != 0)
    {
        ctx->stats.bad_tag++;
        return;
    }

    long v_ver, v_seq, v_rpm, v_torq, v_merr, v_mv, v_mt;
    long v_soc, v_bcurr, v_btemp, v_bvolt, v_bcell, v_berr, v_bv;

    if (!Parse_Int(f[1].p,  f[1].len,   0,  255,        &v_ver))   goto pfail;
    if (!Parse_Int(f[2].p,  f[2].len,   0,  2147483647L,&v_seq))   goto pfail;
    if (!Parse_Int(f[3].p,  f[3].len,   0,  65535,      &v_rpm))   goto pfail;
    if (!Parse_Int(f[4].p,  f[4].len,  -32768, 32767,   &v_torq))  goto pfail;
    if (!Parse_Int(f[5].p,  f[5].len,   0,  255,        &v_merr))  goto pfail;
    if (!Parse_Int(f[6].p,  f[6].len,   0,  1,          &v_mv))    goto pfail;
    if (!Parse_Int(f[7].p,  f[7].len,   0,  1,          &v_mt))    goto pfail;
    if (!Parse_Int(f[8].p,  f[8].len,   0,  255,        &v_soc))   goto pfail;
    if (!Parse_Int(f[9].p,  f[9].len,  -32768, 32767,   &v_bcurr)) goto pfail;
    if (!Parse_Int(f[10].p, f[10].len, -32768, 32767,   &v_btemp)) goto pfail;
    if (!Parse_Int(f[11].p, f[11].len,  0,  65535,      &v_bvolt)) goto pfail;
    if (!Parse_Int(f[12].p, f[12].len,  0,  65535,      &v_bcell)) goto pfail;
    if (!Parse_Int(f[13].p, f[13].len,  0,  255,        &v_berr))  goto pfail;
    if (!Parse_Int(f[14].p, f[14].len,  0,  1,          &v_bv))    goto pfail;

    if ((uint8_t)v_ver != TEL_PROTOCOL_VERSION)
    {
        ctx->stats.bad_version++;
        return;
    }

    /* Sanity range kontrolu */
    if ((uint16_t)v_rpm  > TEL_RPM_MAX      ||
        (uint8_t) v_soc  > TEL_BMS_SOC_MAX  ||
        v_btemp          > TEL_BMS_TEMP_MAX ||
        v_btemp          < TEL_BMS_TEMP_MIN)
    {
        ctx->stats.range_fail++;
        return;
    }

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

/**
 * Tek bir ham byte'i satir tamponuna isler (eski RxBytePush govdesi).
 * v3: Artik ISR'dan DEGIL, Telemetry_Process icinden (main context)
 * cagriliyor. Satir tamamlaninca Decode_Line burada calisir.
 */
static void Process_Byte(TelCtx_t *ctx, uint8_t b, uint32_t now_ms)
{
    ctx->last_rx_ms = now_ms;

    if (b == '\r') return;   /* CR atilir */

    if (b == '\n')
    {
        if (ctx->line_len > 0U)
            Decode_Line(ctx, ctx->line_buf, ctx->line_len);
        ctx->line_len   = 0U;
        ctx->line_state = LINE_IDLE;
        return;
    }

    if (b < 0x20U) return;   /* kontrol karakterleri yoksay */

    if (ctx->line_len < TEL_LINE_MAX_LEN)
    {
        ctx->line_buf[ctx->line_len++] = b;
        ctx->line_state = LINE_COLLECT;
    }
    else
    {
        ctx->stats.overflow_drop++;
        ctx->line_len   = 0U;
        ctx->line_state = LINE_IDLE;
    }
}

/* ========== Encoder ========== */

uint8_t Telemetry_EncodeCommand(uint8_t cmd_byte,
                                uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < 1U) return 0U;
    out_buf[0] = cmd_byte;
    return 1U;
}

uint8_t Telemetry_EncodeEStopBurst(uint8_t *out_buf, size_t max_len)
{
    if (!out_buf || max_len < TEL_ESTOP_BURST_COUNT) return 0U;
    for (uint8_t i = 0; i < TEL_ESTOP_BURST_COUNT; i++)
        out_buf[i] = UKS_CMD_EMERGENCY_STOP;
    return TEL_ESTOP_BURST_COUNT;
}

/* ========== Decoder API ========== */

void Telemetry_Init(TelCtx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->line_state = LINE_IDLE;
}

/**
 * BUG #4 (v3) DUZELTME: ISR yuku.
 * Bu fonksiyon ISR'dan cagrilir ve artik SADECE gelen byte'i ring
 * buffer'a yazar. Parse/tokenize/decode YAPMAZ → ISR sabit-zamanli.
 *
 * Tek-uretici (ISR) / tek-tuketici (Process) deseni: yalnizca rx_head
 * ISR'da yazilir, rx_tail ana donguda. head volatile oldugu icin kilit
 * gerekmez. Ring doluysa byte dusurulur (ring_overflow sayilir) — ama
 * 256 byte tampon 9600 baud'da ~266 ms veri tutar, ana dongu rahatca
 * yetisir.
 *
 * NOT: last_rx_ms burada DEGIL, byte ana donguda islenirken guncellenir
 * (Process_Byte icinde). Boylece timeout saati gercek isleme anini
 * yansitir.
 */
void Telemetry_RxBytePush(TelCtx_t *ctx, uint8_t b, uint32_t now_ms)
{
    (void)now_ms;   /* zaman damgasi ana donguda atilir */
    if (!ctx) return;

    ctx->stats.rx_bytes++;

    uint16_t head = ctx->rx_head;
    uint16_t next = (uint16_t)((head + 1U) & TEL_RX_RING_MASK);

    if (next == ctx->rx_tail)
    {
        /* Ring dolu — ana dongu yetisememis. Byte dusur. */
        ctx->stats.ring_overflow++;
        return;
    }

    ctx->rx_ring[head] = b;
    ctx->rx_head = next;
}

/**
 * v3 YENI: Ring buffer'daki bekleyen tum byte'lari isler.
 * ANA DONGUDEN her tur cagrilmali. Agir is (parse) burada yapilir.
 */
void Telemetry_Process(TelCtx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return;

    /* ISR'in o anki head'ini bir kez oku (volatile snapshot) */
    uint16_t head = ctx->rx_head;

    while (ctx->rx_tail != head)
    {
        uint8_t b = ctx->rx_ring[ctx->rx_tail];
        ctx->rx_tail = (uint16_t)((ctx->rx_tail + 1U) & TEL_RX_RING_MASK);
        Process_Byte(ctx, b, now_ms);
    }
}

uint8_t Telemetry_IsFrameReady(const TelCtx_t *ctx)
{
    return ctx ? ctx->frame_ready : 0U;
}

/**
 * BUG #2 DUZELTME: Telemetry_Parse — frame_ready = 0 atamasi.
 * v3: Commit artik ana baglamda ama PRIMASK korundu (zararsiz, ileride
 * ISR'a tasinirsa guvenli).
 */
TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out)
{
    if (!ctx || !out) return TEL_ERR_NULL;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (!ctx->frame_ready)
    {
        __set_PRIMASK(primask);
        return TEL_NO_DATA;
    }

    *out = ctx->buffers[ctx->read_idx];
    ctx->frame_ready = 0U;

    __set_PRIMASK(primask);
    return TEL_VALID;
}

/**
 * BUG #3 DUZELTME: Telemetry_Tick — LINE_IDLE'da last_rx_ms yanlislikla
 * sifirlaniyordu. IDLE branch'i kaldirildi; last_rx_ms sadece byte
 * islenirken (Process_Byte) guncellenir.
 */
void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return;
    if (ctx->line_state == LINE_IDLE) return;   /* beklenecek bir sey yok */

    if ((now_ms - ctx->last_rx_ms) >= TEL_PARTIAL_TIMEOUT_MS)
    {
        ctx->line_state = LINE_IDLE;
        ctx->line_len   = 0U;
        ctx->stats.timeout_drop++;
    }
}

/* ========== UKS Lokal E-STOP ========== */

uint8_t Telemetry_IsEStopActive(const TelCtx_t *ctx)
{
    return ctx ? ctx->estop_active : 0U;
}

void Telemetry_ClearEStop(TelCtx_t *ctx)
{
    if (ctx) ctx->estop_active = 0U;
}

void Telemetry_SetEStopActive(TelCtx_t *ctx)
{
    if (!ctx) return;
    if (!ctx->estop_active)
    {
        ctx->estop_active = 1U;
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
    const uint8_t bar_w = 20U;
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
           estop_active ? 0U : (unsigned)d->motor_rpm,
           (int)d->motor_torque);
    printf("  |   Errs   : 0x%02X     Valid  : %u  Tout: %u  |\r\n",
           (unsigned)d->motor_error_flags,
           (unsigned)d->motor_data_valid,
           (unsigned)d->motor_timeout_active);
    printf("  |--- BMS ------------------------------------|\r\n");
    printf("  |   SoC    : %3u%%  ", (unsigned)d->bms_soc);
    Print_SocBar(d->bms_soc);
    printf(" |\r\n");

    int      ca = d->bms_current_dA / 10;
    int      cd = d->bms_current_dA % 10; if (cd < 0) cd = -cd;
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
    printf("  Ring overflow   : %lu\r\n", (unsigned long)s->ring_overflow);
    printf("  Gecerli pkt     : %lu\r\n", (unsigned long)s->good_packets);
    printf("  Seq gap         : %lu\r\n", (unsigned long)s->seq_gaps);
    printf("  Seq dup/stale   : %lu\r\n", (unsigned long)s->seq_dup_or_stale);
    printf("  E-STOP TX (UKS) : %lu\r\n", (unsigned long)s->estop_tx_count);
    printf("  E-STOP aktif    : %s\r\n",  ctx->estop_active ? "EVET" : "hayir");
    printf("  ---------------------\r\n\r\n");
}