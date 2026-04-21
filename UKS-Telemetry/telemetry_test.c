/**
 * @file    telemetry_test.c
 * @brief   Byte-stuffing destekli telemetri modulu unit testleri.
 *
 *  Derleme:
 *      gcc -Wall -Wextra -pedantic -std=c11 \
 *          telemetry.c telemetry_test.c -o telemetry_test
 *  Calistirma:
 *      ./telemetry_test
 */

#include "telemetry.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Basit Test Framework'u ---- */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(actual, expected, msg)                                  \
    do {                                                                  \
        tests_run++;                                                      \
        if ((actual) == (expected)) {                                      \
            tests_passed++;                                               \
        } else {                                                          \
            tests_failed++;                                               \
            printf("  FAIL [%s:%d] %s\n"                                  \
                   "       beklenen: %d  gerceklesen: %d\n",              \
                   __FILE__, __LINE__, (msg),                             \
                   (int)(expected), (int)(actual));                        \
        }                                                                 \
    } while (0)

#define RUN_TEST(fn)                                                      \
    do {                                                                  \
        printf(">> %s\n", #fn);                                           \
        fn();                                                             \
    } while (0)

/* ---- Yardimci: Encode edip aliciya bas ---- */

static void encode_and_push(TelCtx_t *ctx, const TelData_t *data)
{
    uint8_t wire[TEL_STUFFED_MAX_LEN];
    uint8_t len = Telemetry_Encode(data, wire);

    for (uint8_t i = 0; i < len; i++)
    {
        Telemetry_RxBytePush(ctx, wire[i]);
    }
}

static void push_bytes(TelCtx_t *ctx, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        Telemetry_RxBytePush(ctx, buf[i]);
    }
}

/* ================================================================ */
/*                       TEST FONKSIYONLARI                        */
/* ================================================================ */

/* 1. Normal paket: encode -> push -> parse */
static void test_basic_roundtrip(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 85, .soc = 72, .battery_temp = 35, .motor_rpm = 3000 };
    encode_and_push(&ctx, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame hazir olmali");

    TelData_t rx;
    TelStatus_t s = Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(s,              TEL_VALID, "status TEL_VALID");
    ASSERT_EQ(rx.speed,       85,        "speed = 85");
    ASSERT_EQ(rx.soc,         72,        "soc = 72");
    ASSERT_EQ(rx.battery_temp, 35,       "bat_temp = 35");
    ASSERT_EQ(rx.motor_rpm,   3000,      "rpm = 3000");
    ASSERT_EQ(ctx.stats.good_packets, 1, "good_packets = 1");
}

/* 2. Veri icinde 0xAA olan paket (stuffing testi) */
static void test_stuff_header_in_data(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* speed = 0xAA -> stuff edilmeli */
    TelData_t tx = { .speed = 0xAA, .soc = 50, .battery_temp = 25, .motor_rpm = 1000 };
    encode_and_push(&ctx, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame hazir");

    TelData_t rx;
    TelStatus_t s = Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(s,        TEL_VALID, "stuffed 0xAA gecerli parse edilmeli");
    ASSERT_EQ(rx.speed, 0xAA,     "speed = 0xAA (170)");
    ASSERT_EQ(rx.soc,   50,       "soc = 50");
}

/* 3. Veri icinde 0xAB olan paket (escape byte stuff testi) */
static void test_stuff_esc_in_data(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* speed = 0xAB -> stuff edilmeli (speed siniri 255, sorun yok) */
    TelData_t tx = { .speed = 0xAB, .soc = 50, .battery_temp = 20, .motor_rpm = 500 };
    encode_and_push(&ctx, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame hazir");

    TelData_t rx;
    TelStatus_t s = Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(s,        TEL_VALID, "stuffed 0xAB gecerli parse edilmeli");
    ASSERT_EQ(rx.speed, 0xAB,     "speed = 0xAB (171)");
}

/* 4. Tum payload byte'lari 0xAA (worst case stuffing) */
static void test_all_bytes_are_header(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 0xAA, .soc = 0xAA, .battery_temp = 0xAA, .motor_rpm = 0xAAAA };

    uint8_t wire[TEL_STUFFED_MAX_LEN];
    uint8_t len = Telemetry_Encode(&tx, wire);

    /* Worst case: her payload byte'i 2 byte olur -> 1 + 6*2 = 13 */
    /* Ama checksum da stuff edilebilir, kontrol edelim */
    ASSERT_EQ(wire[0], TEL_HEADER_BYTE, "ilk byte header olmali");

    /* Her sey stuff edilmis olsa bile roundtrip calismali */
    for (uint8_t i = 0; i < len; i++)
        Telemetry_RxBytePush(&ctx, wire[i]);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame hazir");

    TelData_t rx;
    Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(rx.speed,     0xAA,   "speed = 0xAA");
    ASSERT_EQ(rx.soc,       0xAA,   "soc = 0xAA");
    ASSERT_EQ(rx.motor_rpm, 0xAAAA, "rpm = 0xAAAA");
}

/* 5. Bos buffer */
static void test_empty_buffer(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 0, "bos buffer — frame yok");

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_NO_DATA, "TEL_NO_DATA");
}

/* 6. Checksum hatasi */
static void test_checksum_fail(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 50, .soc = 80, .battery_temp = 25, .motor_rpm = 2000 };
    uint8_t wire[TEL_STUFFED_MAX_LEN];
    uint8_t len = Telemetry_Encode(&tx, wire);

    /* Son byte'i boz (stuffed olmayan bir degere) */
    wire[len - 1] ^= 0xFF;

    push_bytes(&ctx, wire, len);

    /* Frame tamamlanir ama checksum tutmaz */
    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame geldi");
    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_CHK_FAIL, "checksum hatasi");
    ASSERT_EQ(ctx.stats.chk_fail, 1, "chk_fail = 1");
}

/* 7. SoC sinir disi */
static void test_out_of_range_soc(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 40, .soc = 120, .battery_temp = 25, .motor_rpm = 1000 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    TelStatus_t s = Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(s, TEL_OUT_OF_RANGE, "soc=120 sinir disi");
    ASSERT_EQ(rx.soc, 120, "struct yine doldurulmali");
    ASSERT_EQ(ctx.stats.range_fail, 1, "range_fail = 1");
}

/* 8. RPM sinir disi */
static void test_out_of_range_rpm(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 40, .soc = 50, .battery_temp = 25, .motor_rpm = 20000 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_OUT_OF_RANGE, "rpm=20000 sinir disi");
}

/* 9. bat_temp sinir disi */
static void test_out_of_range_bat_temp(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 40, .soc = 50, .battery_temp = 95, .motor_rpm = 1000 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_OUT_OF_RANGE, "bat_temp=95 sinir disi");
}

/* 10. Art arda iki paket */
static void test_two_packets(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx1 = { .speed = 10, .soc = 20, .battery_temp = 30, .motor_rpm = 400 };
    TelData_t tx2 = { .speed = 90, .soc = 80, .battery_temp = 40, .motor_rpm = 5000 };

    encode_and_push(&ctx, &tx1);

    /* Paket 1 */
    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "pkt1 hazir");
    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "pkt1 gecerli");
    ASSERT_EQ(rx.speed, 10, "pkt1 speed = 10");

    encode_and_push(&ctx, &tx2);

    /* Paket 2 */
    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "pkt2 hazir");
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "pkt2 gecerli");
    ASSERT_EQ(rx.speed, 90, "pkt2 speed = 90");

    ASSERT_EQ(ctx.stats.good_packets, 2, "good_packets = 2");
}

/* 11. Cop byte'lar + gecerli paket */
static void test_garbage_before_packet(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Header olmayan cop byte'lar — decoder IDLE'da bunlari yoksayar */
    uint8_t garbage[] = { 0x01, 0x02, 0xFF, 0x55, 0x33 };
    push_bytes(&ctx, garbage, sizeof(garbage));

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 0, "cop sonrasi frame yok");

    /* Simdi gecerli paket */
    TelData_t tx = { .speed = 60, .soc = 50, .battery_temp = 30, .motor_rpm = 1500 };
    encode_and_push(&ctx, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "frame hazir");

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "cop sonrasi gecerli");
    ASSERT_EQ(rx.speed, 60, "speed = 60");
}

/* 12. Eksik paket + yeni header (otomatik reset) */
static void test_interrupted_frame(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Yarim paket gonder: header + 3 byte */
    uint8_t partial[] = { TEL_HEADER_BYTE, 0x10, 0x20, 0x30 };
    push_bytes(&ctx, partial, sizeof(partial));

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 0, "yarim frame — hazir degil");

    /* Yeni header ile gercek paket gelsin */
    TelData_t tx = { .speed = 33, .soc = 44, .battery_temp = 22, .motor_rpm = 800 };
    encode_and_push(&ctx, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 1, "yeni frame hazir");

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "yeni frame gecerli");
    ASSERT_EQ(rx.speed, 33, "speed = 33");
}

/* 13. Timeout testi */
static void test_partial_timeout(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Header + 2 byte (eksik) */
    uint8_t partial[] = { TEL_HEADER_BYTE, 0x10, 0x20 };
    push_bytes(&ctx, partial, sizeof(partial));

    /* Decoder FRAME_PAYLOAD durumunda olmali */
    Telemetry_Tick(&ctx, 0);
    Telemetry_Tick(&ctx, TEL_PARTIAL_TIMEOUT_MS - 1);
    ASSERT_EQ(ctx.stats.timeout_drop, 0, "timeout oncesi drop yok");

    Telemetry_Tick(&ctx, TEL_PARTIAL_TIMEOUT_MS);
    ASSERT_EQ(ctx.stats.timeout_drop, 1, "timeout sonrasi drop = 1");
}

/* 14. Gecersiz escape dizisi */
static void test_invalid_escape_sequence(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Header + ESC + gecersiz kod (0x99) */
    uint8_t bad[] = { TEL_HEADER_BYTE, TEL_ESC_BYTE, 0x99 };
    push_bytes(&ctx, bad, sizeof(bad));

    ASSERT_EQ(ctx.stats.stuff_err, 1, "stuff_err = 1");
    ASSERT_EQ(Telemetry_IsFrameReady(&ctx), 0, "frame olusmamal");
}

/* 15. Stats reset */
static void test_stats_reset(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 50, .soc = 50, .battery_temp = 25, .motor_rpm = 1000 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    Telemetry_Parse(&ctx, &rx);

    ASSERT_EQ(ctx.stats.good_packets, 1, "parse sonrasi = 1");

    Telemetry_ResetStats(&ctx);
    ASSERT_EQ(ctx.stats.good_packets, 0, "reset sonrasi = 0");
    ASSERT_EQ(ctx.stats.rx_bytes,     0, "rx_bytes = 0");
}

/* 16. Coklu instance bagimsizlik */
static void test_multiple_instances(void)
{
    TelCtx_t a, b;
    Telemetry_Init(&a);
    Telemetry_Init(&b);

    TelData_t tx = { .speed = 77, .soc = 55, .battery_temp = 20, .motor_rpm = 2200 };
    encode_and_push(&a, &tx);

    ASSERT_EQ(Telemetry_IsFrameReady(&b), 0, "b bos");
    ASSERT_EQ(Telemetry_IsFrameReady(&a), 1, "a hazir");

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&a, &rx), TEL_VALID, "a gecerli");
    ASSERT_EQ(rx.speed, 77, "a speed = 77");
    ASSERT_EQ(b.stats.rx_bytes, 0, "b rx_bytes = 0");
}

/* 17. Sinir degerler (tam sinirda — gecerli olmali) */
static void test_boundary_valid(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 255, .soc = 100, .battery_temp = 80, .motor_rpm = 15000 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "sinir degerler gecerli");
    ASSERT_EQ(rx.soc, 100,       "soc = 100");
    ASSERT_EQ(rx.motor_rpm, 15000, "rpm = 15000");
}

/* 18. Sifir degerler */
static void test_zero_values(void)
{
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    TelData_t tx = { .speed = 0, .soc = 0, .battery_temp = 0, .motor_rpm = 0 };
    encode_and_push(&ctx, &tx);

    TelData_t rx;
    ASSERT_EQ(Telemetry_Parse(&ctx, &rx), TEL_VALID, "sifir degerler gecerli");
    ASSERT_EQ(rx.speed, 0, "speed = 0");
    ASSERT_EQ(rx.motor_rpm, 0, "rpm = 0");
}

/* 19. Encode cikti boyutu dogrulama */
static void test_encode_length(void)
{
    uint8_t wire[TEL_STUFFED_MAX_LEN];

    /* Stuff gerektirmeyen paket: 1 header + 6 payload = 7 */
    TelData_t tx1 = { .speed = 10, .soc = 20, .battery_temp = 30, .motor_rpm = 400 };
    uint8_t len1 = Telemetry_Encode(&tx1, wire);
    /* Checksum de stuff gerektirmiyorsa 7 olmali, aksi halde 8 */
    /* Kesin degeri bilemeyiz ama 7..13 arasinda olmali */
    ASSERT_EQ(len1 >= 7 && len1 <= TEL_STUFFED_MAX_LEN, 1, "uzunluk 7..13 arasi");

    /* Wire'in ilk byte'i her zaman header olmali */
    ASSERT_EQ(wire[0], TEL_HEADER_BYTE, "ilk byte header");
}

/* 20. Print fonksiyonlari (crash testi — ciktiya bakilir) */
static void test_print_functions(void)
{
    TelData_t data = { .speed = 85, .soc = 72, .battery_temp = 35, .motor_rpm = 3000 };

    printf("  -- Compact format --\n  ");
    Telemetry_PrintCompact(&data, TEL_VALID);

    printf("  -- Compact (hata) --\n  ");
    Telemetry_PrintCompact(&data, TEL_CHK_FAIL);

    printf("  -- Compact (no data) --\n  ");
    Telemetry_PrintCompact(&data, TEL_NO_DATA);

    /* Dashboard: normal */
    Telemetry_PrintDashboard(&data, TEL_VALID);

    /* Dashboard: yuksek sicaklik */
    TelData_t hot = { .speed = 50, .soc = 30, .battery_temp = 65, .motor_rpm = 4000 };
    Telemetry_PrintDashboard(&hot, TEL_VALID);

    /* Dashboard: uyari sicaklik */
    TelData_t warm = { .speed = 50, .soc = 30, .battery_temp = 50, .motor_rpm = 4000 };
    Telemetry_PrintDashboard(&warm, TEL_VALID);

    /* Dashboard: no data */
    Telemetry_PrintDashboard(&data, TEL_NO_DATA);

    /* Stats */
    TelCtx_t ctx;
    Telemetry_Init(&ctx);
    encode_and_push(&ctx, &data);
    TelData_t rx;
    Telemetry_Parse(&ctx, &rx);
    Telemetry_PrintStats(&ctx);

    /* Crash olmadiysa basarili */
    tests_run++;
    tests_passed++;
    printf("  print fonksiyonlari OK (crash yok)\n");
}

/* ================================================================ */
/*                              MAIN                                */
/* ================================================================ */

int main(void)
{
    printf("\n============================================\n");
    printf("   TELEMETRI MODUL UNIT TESTLERI (v2)\n");
    printf("   Byte-Stuffing + Yer Istasyonu Print\n");
    printf("============================================\n\n");

    RUN_TEST(test_basic_roundtrip);
    RUN_TEST(test_stuff_header_in_data);
    RUN_TEST(test_stuff_esc_in_data);
    RUN_TEST(test_all_bytes_are_header);
    RUN_TEST(test_empty_buffer);
    RUN_TEST(test_checksum_fail);
    RUN_TEST(test_out_of_range_soc);
    RUN_TEST(test_out_of_range_rpm);
    RUN_TEST(test_out_of_range_bat_temp);
    RUN_TEST(test_two_packets);
    RUN_TEST(test_garbage_before_packet);
    RUN_TEST(test_interrupted_frame);
    RUN_TEST(test_partial_timeout);
    RUN_TEST(test_invalid_escape_sequence);
    RUN_TEST(test_stats_reset);
    RUN_TEST(test_multiple_instances);
    RUN_TEST(test_boundary_valid);
    RUN_TEST(test_zero_values);
    RUN_TEST(test_encode_length);
    RUN_TEST(test_print_functions);

    printf("\n============================================\n");
    printf("  SONUC: %d / %d test gecti", tests_passed, tests_run);
    if (tests_failed > 0)
        printf("  (%d BASARISIZ)", tests_failed);
    printf("\n============================================\n\n");

    return (tests_failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
