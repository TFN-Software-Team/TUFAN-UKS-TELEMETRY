/**
 * @file    telemetry.c
 * @brief   AKS uyumlu ASCII CSV decoder + tek-byte komut encoder.
 *
 *  Duzeltmeler (v2 — NOT: 1-2. maddeler v4'te kuyruk yapisiyla
 *  tasinmistir, asagidaki v4 bolumune bakiniz):
 *  3) Telemetry_Tick: LINE_IDLE durumunda last_rx_ms sifirlanmiyordu,
 *  3) Telemetry_Tick: LINE_IDLE durumunda last_rx_ms sifirlanmiyordu,
 *     bu timeout saatini yanlis sifirliyor ve LINE_COLLECT'e girer
 *     girmez timeout'u uzatiyordu. IDLE branch'i kaldirildi.
 * 
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
 *     NOT: Decode_Line / Parse_Int / Tokenize / Track_Sequence mantigi
 *     DEGISMEDI — yalnizca cagrildiklari baglam (ISR → main) degisti.
 *
 *  Duzeltmeler (v4 — frame kuyrugu):
 *  5) Cift tampon (buffers[2] + frame_ready) yerine SPSC frame kuyrugu
 *     (frame_q[TEL_FRAME_Q_DEPTH]). Eski tasarimda tek main turunda iki
 *     satir decode edilirse ikincisi dusuyordu (queue_overflow_drop). Kuyruk
 *     derinligi ile bu kayip pratikte sifirlanir.
 *  6) Commit_Frame artik basari (1/0) dondurur; good_packets yalnizca
 *     kuyruga gercekten yayinlanan frame icin artar (eski kodda dusen
 *     frame de good sayiliyordu).
 *  7) PRIMASK kritik bolumleri kaldirildi: uretici (Process) ve tuketici
 *     (Parse) ana baglamda, SPSC → kilit gereksiz (rx_ring ile ayni
 *     gerekce). stm32f1xx.h include'i artik gerekli degil.
 */

#include "telemetry.h"
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
 * Isaretsiz ondalik tam sayi parser'i. '+'/'-' kabul etmez (yalnizca rakam).
 * Donus: 1=basarili, 0=hata. UINT32_MAX'a (4294967295) kadar tasma-guvenli.
 *
 * Neden ayri fonksiyon: Parse_Int'in birikimi `long` (isaretli 32-bit) —
 * ts_ms/seq gibi AKS'te uint32 olarak tanimli alanlar 2147483647'i
 * astiginda Parse_Int'e sigmiyor ve paket sessizce reddediliyordu (bkz.
 * UYUM_NOTU.md). Parse_U32 birikimi `uint32_t` tutarak tam UINT32_MAX
 * araligini kabul eder.
 */
static int Parse_U32(const char *s, uint16_t len,
                     uint32_t min_v, uint32_t max_v, uint32_t *out)
{
    if (len == 0) return 0;

    uint32_t v = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9') return 0;
        /* Parse_Int'teki son-hane tasma korumasinin isaretsiz kariligi:
         * v==429496729 iken digit>5 gelirse v*10+digit = 4294967296+ olur
         * ki bu UINT32_MAX'i asar (tasma degil, UB de degil — isaretsiz
         * aritmetik mod 2^32 sarar — ama yanlis/kucuk bir deger sessizce
         * KABUL edilmis olurdu). Son hane onceden kontrol edilerek bu
         * yanlis-kabul onlenir. */
        if (v > 429496729U ||
            (v == 429496729U && (uint32_t)(s[i] - '0') > 5U)) return 0;
        v = v * 10U + (uint32_t)(s[i] - '0');
    }
    if (v < min_v || v > max_v) return 0;
    *out = v;
    return 1;
}

/**
 * Commit_Frame — decode edilmis frame'i SPSC kuyruga yayinlar.
 *
 * Decode_Line frame'i zaten &ctx->frame_q[fq_head]'e yazdi; burada sadece
 * doluluk kontrolu yapilip head ilerletilir (rx_ring enqueue deseninin
 * aynisi). Kuyruk doluysa en-yeni frame dusurulur (drop-newest) ve head
 * ilerletilmez → o slot bir sonraki decode'da uzerine yazilir.
 *
 * Donus: 1 = yayinlandi, 0 = kuyruk dolu, dusuruldu.
 *
 * v3 NOT: Hem uretici (Process) hem tuketici (Parse) ana baglamda;
 * SPSC oldugu icin kritik bolum (PRIMASK) GEREKMEZ. fq_head/fq_tail
 * volatile yeterli — rx_ring ile ayni gerekce.
 */
static inline uint8_t Commit_Frame(TelCtx_t *ctx)
{
    uint8_t next = (uint8_t)((ctx->fq_head + 1U) & TEL_FRAME_Q_MASK);

    if (next == ctx->fq_tail)
    {
        ctx->stats.queue_overflow_drop++;  /* kuyruk dolu — ana dongu yetisemedi */
        return 0U;
    }

    ctx->fq_head = next;              /* yayinla */
    return 1U;
}

/**
 * Sequence numarasinda gap / duplicate / stale tespiti.
 *
 * Tasma/sarma notu: `seq - ctx->last_sequence` iki uint32_t arasinda
 * ISARETSIZ aritmetiktir (mod 2^32 sarar, UB YOK); sonucun (int32_t)'a
 * cast'i "ileri mi geri mi" yonunu okumak icin kullanilir — bu, klasik
 * TCP sequence-wraparound karsilastirma deseni ve tanimsiz davranis
 * icermez (GCC/Clang'da implementation-defined, deterministik iki'nin
 * tumleyeni donusumudur). `ctx->last_sequence + 1U` de ayni sekilde
 * isaretsiz sarar. Sonuc: seq UINT32_MAX (4294967295) -> 0 sardiginda
 * (uint32 ts/seq alaninin dogal sarma noktasi) bu, YANLIS bir dev "gap"
 * olarak DEGIL, dogru sekilde "bir sonraki ardisik seq" olarak
 * algilanir — cunku hem fark hem +1 ayni modulo aritmetigi kullanir.
 */
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

    /* Alan sirasi (TEL haric, 0-index'li f[]):
     *  1=ver 2=seq 3=rpm 4=torque 5=motorErr 6=motorValid 7=motorTimeout
     *  8=cellVMax 9=cellVMin 10=tempH 11=tempL 12=sysState 13=packV
     *  14=current 15=soc 16=bmsValid 17=ts_ms 18=spd_x10 */
    long v_ver, v_rpm, v_torq, v_merr, v_mv, v_mt;
    long v_cellmax, v_cellmin, v_temph, v_templ, v_sysst;
    long v_packv, v_curr, v_soc, v_bv, v_spd;
    /* seq/ts_ms AKS'te uint32'dir ve sarabilir; `long` (isaretli 32-bit)
     * 2147483647'nin ustunu tutamayacagindan bunlar ayrica uint32_t
     * ve Parse_U32 ile tutulur (bkz. Parse_U32 yorumu, UYUM_NOTU.md). */
    uint32_t v_seq, v_tsms;

    if (!Parse_Int(f[1].p,  f[1].len,   0,  255,        &v_ver))     goto pfail;
    if (!Parse_U32(f[2].p,  f[2].len,   0U, UINT32_MAX, &v_seq))     goto pfail;
    if (!Parse_Int(f[3].p,  f[3].len,   0,  65535,      &v_rpm))     goto pfail;
    if (!Parse_Int(f[4].p,  f[4].len,  -32768, 32767,   &v_torq))    goto pfail;
    if (!Parse_Int(f[5].p,  f[5].len,   0,  255,        &v_merr))    goto pfail;
    if (!Parse_Int(f[6].p,  f[6].len,   0,  1,          &v_mv))      goto pfail;
    if (!Parse_Int(f[7].p,  f[7].len,   0,  1,          &v_mt))      goto pfail;
    if (!Parse_Int(f[8].p,  f[8].len,   0,  65535,      &v_cellmax)) goto pfail;
    if (!Parse_Int(f[9].p,  f[9].len,   0,  65535,      &v_cellmin)) goto pfail;
    if (!Parse_Int(f[10].p, f[10].len, -128, 127,       &v_temph))   goto pfail;
    if (!Parse_Int(f[11].p, f[11].len, -128, 127,       &v_templ))   goto pfail;
    if (!Parse_Int(f[12].p, f[12].len,  1,  4,          &v_sysst))   goto pfail;
    if (!Parse_Int(f[13].p, f[13].len,  0,  65535,      &v_packv))   goto pfail;
    /* INT32_MIN bilinçli olarak dışlanır; AKS bu değeri üretmez (sanitize
     * eder). Bkz. ESP_AKS TelemetrySanitize. */
    if (!Parse_Int(f[14].p, f[14].len, -2147483647L, 2147483647L,
                                                         &v_curr))   goto pfail;
    if (!Parse_Int(f[15].p, f[15].len,  0,  10000,      &v_soc))     goto pfail;
    if (!Parse_Int(f[16].p, f[16].len,  0,  1,          &v_bv))      goto pfail;
    if (!Parse_U32(f[17].p, f[17].len,  0U, UINT32_MAX, &v_tsms))    goto pfail;
    if (!Parse_Int(f[18].p, f[18].len,  0,  3000,       &v_spd))     goto pfail;

    if ((uint8_t)v_ver != TEL_PROTOCOL_VERSION)
    {
        ctx->stats.bad_version++;
        return;
    }

    /* Sanity range kontrolu (tip/Parse_Int sinirinin OTESINDE ek kontrol) */
    if ((uint16_t)v_rpm > TEL_RPM_MAX)
    {
        ctx->stats.range_fail++;
        return;
    }

    /* FIX-F NOT: Track_Sequence BILEREK Commit_Frame'den ONCE ve ondan
     * BAGIMSIZ cagrilir. Sequence izlemesi RF/hat sagligini olcer — yani
     * "hat uzerinde gecerli olarak GORULEN" frame akisini. Eger izleme
     * Commit basarisina baglansaydi, kuyruk doldugunda (ana dongu gecici
     * yetisemediginde) izleme durur, kuyruk bosalinca da dusen frame'ler
     * sahte bir seq_gap olarak raporlanirdi — bu, RF hattini saglikliyken
     * arizali gosterir. Mevcut sira dogrudur: gecerli parse edilen her frame
     * sequence'a sayilir; kuyruk doluluğu ayrica queue_overflow_drop'ta izlenir.
     * Telemetri VERISI bundan etkilenmez; yalnizca istatistik semantigi. */
    Track_Sequence(ctx, v_seq);

    /* Kuyrukta bir sonraki bos slot'a (fq_head) yaz */
    TelData_t *d = &ctx->frame_q[ctx->fq_head];
    d->protocol_version     = (uint8_t) v_ver;
    d->sequence              = v_seq;
    d->motor_rpm             = (uint16_t)v_rpm;
    d->motor_torque          = (int16_t) v_torq;
    d->motor_error_flags     = (uint8_t) v_merr;
    d->motor_data_valid      = (uint8_t) v_mv;
    d->motor_timeout_active  = (uint8_t) v_mt;
    d->bms_cell_vmax_decimv  = (uint16_t)v_cellmax;
    d->bms_cell_vmin_decimv  = (uint16_t)v_cellmin;
    d->bms_temp_highest_c    = (int16_t) v_temph;
    d->bms_temp_lowest_c     = (int16_t) v_templ;
    d->bms_system_state      = (uint8_t) v_sysst;
    d->bms_pack_voltage_deciv = (uint16_t)v_packv;
    d->bms_current_centima   = (int32_t) v_curr;
    d->bms_soc_hundredths    = (uint16_t)v_soc;
    d->bms_data_valid        = (uint8_t) v_bv;
    d->timestamp_ms          = v_tsms;
    d->speed_kmh_x10         = (uint16_t)v_spd;

    /* Yalnizca kuyruga gercekten yayinlanabilen frame "good" sayilir.
     * Dolu kuyrukta dusen frame Commit_Frame icinde queue_overflow_drop'a yazilir. */
    if (Commit_Frame(ctx))
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

    /* LINE_OVERFLOW: tampon tasan bir satirin kuyruğundayiz.
     * \n gorene kadar her byte'i koşulsuz cop'e at. \n gelince
     * satir bitti, temiz bir sonraki frame'e hazirlan.
     * Olmadan: tasan frame'in kuyrugu LINE_IDLE'da yeni frame
     * baslangiç saniyor, \n gelince Decode_Line'a cop gonderiyor
     * -> parse_fail flood + CPU israfi. */
    if (ctx->line_state == LINE_OVERFLOW)
    {
        if (b == '\n')
        {
            ctx->line_len   = 0U;
            ctx->line_state = LINE_IDLE;
        }
        return;   /* overflow bitmeden hicbir seyi isleme */
    }

    if (b == '\r') return;   /* CR atilir */

    if (b == '\n')
    {
        if (ctx->line_len > 0U && ctx->line_state == LINE_COLLECT)
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
        /* Tampon doldu. LINE_OVERFLOW'a gec; bu satirin geri kalan
         * byte'lari bir sonraki \n'e kadar sessizce cop'e atilacak. */
        ctx->stats.line_overflow_drop++;
        ctx->line_len   = 0U;
        ctx->line_state = LINE_OVERFLOW;
    }
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
    return ctx ? (uint8_t)(ctx->fq_head != ctx->fq_tail) : 0U;
}

/**
 * Telemetry_Parse — kuyrugun en eski frame'ini cikar (FIFO).
 * SPSC tuketicisi: yalnizca fq_tail'i ilerletir. Kilit gerekmez.
 */
TelStatus_t Telemetry_Parse(TelCtx_t *ctx, TelData_t *out)
{
    if (!ctx || !out) return TEL_ERR_NULL;

    if (ctx->fq_head == ctx->fq_tail)
        return TEL_NO_DATA;            /* kuyruk bos */

    *out = ctx->frame_q[ctx->fq_tail];
    ctx->fq_tail = (uint8_t)((ctx->fq_tail + 1U) & TEL_FRAME_Q_MASK);
    return TEL_VALID;
}

/**
 * BUG #3 DUZELTME: Telemetry_Tick — LINE_IDLE'da last_rx_ms yanlislikla
 * sifirlaniyordu. IDLE branch'i kaldirildi; last_rx_ms sadece byte
 * islenirken (Process_Byte) guncellenir.
 *
 * LINE_OVERFLOW icin ek not: overflow surecinde timeout gelirse satiri
 * terk edip IDLE'a donmek dogru. Process_Byte'taki overflow akisi \n
 * bekler ama hat kesilirse \n gelmeyebilir; timeout bunu temizler.
 */
void Telemetry_Tick(TelCtx_t *ctx, uint32_t now_ms)
{
    if (!ctx) return;
    /* LINE_IDLE: beklenecek bir sey yok. LINE_OVERFLOW: aktifte sayilir,
     * timeout ile temizlenebilir. */
    if (ctx->line_state == LINE_IDLE) return;

    if ((now_ms - ctx->last_rx_ms) >= TEL_PARTIAL_TIMEOUT_MS)
    {
        ctx->line_state = LINE_IDLE;
        ctx->line_len   = 0U;
        ctx->stats.timeout_drop++;
    }
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

static const char *SysState_Str(uint8_t st)
{
    switch (st)
    {
        case 1: return "DISCHARGE";
        case 2: return "IDLE";
        case 3: return "CHARGE";
        case 4: return "FAULT";
        default: return "UNK";
    }
}

void Telemetry_PrintDashboard(const TelData_t *d, TelStatus_t status,
                              uint8_t link_down, uint32_t queue_overflow_drop)
{
    printf("\r\n");
    printf("  +============================================+\r\n");
    printf("  |        UKS YER ISTASYONU TELEMETRI         |\r\n");
    printf("  +============================================+\r\n");
    printf("  |  LINK: %-36s|\r\n", link_down ? "DOWN" : "OK");
    printf("  |  Kuyruk overflow: %-26lu|\r\n",
           (unsigned long)queue_overflow_drop);

    if (status == TEL_NO_DATA || !d)
    {
        printf("  |  ** AKS'ten veri bekleniyor...            |\r\n");
        printf("  +============================================+\r\n\r\n");
        return;
    }

    printf("  |  Durum: %-7s  Seq: %-8lu Ver: %u       |\r\n",
           Status_Str(status), (unsigned long)d->sequence,
           (unsigned)d->protocol_version);
    printf("  |  t=%lu ms   hiz: %u.%u km/h                  |\r\n",
           (unsigned long)d->timestamp_ms,
           (unsigned)(d->speed_kmh_x10 / 10U),
           (unsigned)(d->speed_kmh_x10 % 10U));
    printf("  |--- Motor ----------------------------------|\r\n");
    printf("  |   RPM    : %5u    Torque : %6d         |\r\n",
           (unsigned)d->motor_rpm, (int)d->motor_torque);
    printf("  |   Errs   : 0x%02X     Valid  : %u  Tout: %u  |\r\n",
           (unsigned)d->motor_error_flags,
           (unsigned)d->motor_data_valid,
           (unsigned)d->motor_timeout_active);
    printf("  |--- BMS (%-9s, V:%u) ----------------|\r\n",
           SysState_Str(d->bms_system_state),
           (unsigned)d->bms_data_valid);

    unsigned soc_pct  = (unsigned)(d->bms_soc_hundredths / 100U);
    unsigned soc_frac = (unsigned)(d->bms_soc_hundredths % 100U);
    printf("  |   SoC    : %3u.%02u%%  ", soc_pct, soc_frac);
    Print_SocBar((uint8_t)soc_pct);
    printf(" |\r\n");

    // NOT: bms_current_centima birimi centi-Amper (0.01 A)'dir (AKS
    // TEL_bmsCurrentCentiA ile ayni) — /100 ve %100 zaten dogru Amper +
    // 2-ondalik kesir uretiyor. Etiket ONCEDEN yanlislikla "mA" yaziyordu
    // (deger dogruydu, yalnizca birim etiketi 1000x yanilticiydi).
    long     ca = d->bms_current_centima / 100;
    long     cd = d->bms_current_centima % 100; if (cd < 0) cd = -cd;
    unsigned va = (unsigned)(d->bms_pack_voltage_deciv / 10U);
    unsigned vd = (unsigned)(d->bms_pack_voltage_deciv % 10U);
    printf("  |   Curr   : %6ld.%02ld A   Pack : %3u.%u V      |\r\n",
           ca, cd, va, vd);

    printf("  |   TempH  : %4d C   TempL : %4d C",
           (int)d->bms_temp_highest_c, (int)d->bms_temp_lowest_c);
    if      (d->bms_temp_highest_c > 60) printf("  !! YUKSEK !!");
    else if (d->bms_temp_highest_c > 45) printf("  !  UYARI  !");
    else                                 printf("             ");
    printf(" |\r\n");

    unsigned cmax_i = (unsigned)(d->bms_cell_vmax_decimv / 10U);
    unsigned cmax_f = (unsigned)(d->bms_cell_vmax_decimv % 10U);
    unsigned cmin_i = (unsigned)(d->bms_cell_vmin_decimv / 10U);
    unsigned cmin_f = (unsigned)(d->bms_cell_vmin_decimv % 10U);
    printf("  |   CellMax: %4u.%u mV  CellMin: %4u.%u mV  |\r\n",
           cmax_i, cmax_f, cmin_i, cmin_f);
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
    printf("  Timeout drop    : %lu\r\n", (unsigned long)s->timeout_drop);
    printf("  Satir overflow  : %lu\r\n", (unsigned long)s->line_overflow_drop);
    printf("  Kuyruk overflow : %lu\r\n", (unsigned long)s->queue_overflow_drop);
    printf("  Ring overflow   : %lu\r\n", (unsigned long)s->ring_overflow);
    printf("  Gecerli pkt     : %lu\r\n", (unsigned long)s->good_packets);
    printf("  Seq gap         : %lu\r\n", (unsigned long)s->seq_gaps);
    printf("  Seq dup/stale   : %lu\r\n", (unsigned long)s->seq_dup_or_stale);
    printf("  ---------------------\r\n\r\n");
}