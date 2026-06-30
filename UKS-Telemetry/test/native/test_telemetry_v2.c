/**
 * @file    test_telemetry_v2.c
 * @brief   UKS telemetri parser (v2, 19 alan) icin host-native testler.
 *
 *  telemetry.c HAL'a bagimli degildir (yalnizca stdint/stddef/stdio/string),
 *  bu yuzden hedef MCU'ya gerek olmadan dogrudan gcc ile derlenip
 *  PC uzerinde calistirilabilir.
 *
 *  Derleme / calistirma (UKS-Telemetry/test/native icinden):
 *      gcc -std=c99 -Wall -Wextra -I../../Core/Inc \
 *          test_telemetry_v2.c ../../Core/Src/telemetry.c \
 *          -o test_telemetry_v2 && ./test_telemetry_v2
 */

#include "telemetry.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            printf("  [FAIL] %s (line %d)\n", msg, __LINE__);             \
        }                                                                 \
    } while (0)

/** Bir CSV satirini (CRLF eklenerek) ctx'e besler ve ring/process'i tetikler. */
static void Feed_Line(TelCtx_t *ctx, const char *line, uint32_t now_ms)
{
    size_t len = strlen(line);
    for (size_t i = 0; i < len; i++)
        Telemetry_RxBytePush(ctx, (uint8_t)line[i], now_ms);
    Telemetry_RxBytePush(ctx, (uint8_t)'\r', now_ms);
    Telemetry_RxBytePush(ctx, (uint8_t)'\n', now_ms);
    Telemetry_Process(ctx, now_ms);
}

/* ===================================================================== */

static void test_real_aks_vector(void)
{
    printf("test_real_aks_vector...\n");
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Gercek AKS test vektoru (gorev tanimindan, ts_ms/spd_x10 dolduruldu) */
    const char *line =
        "TEL,2,0,1500,-250,5,1,0,37734,37422,32,31,2,780,-181610,6283,1,123456,425";
    Feed_Line(&ctx, line, 1000U);

    CHECK(Telemetry_IsFrameReady(&ctx) != 0U, "frame should be ready");

    TelData_t d;
    TelStatus_t st = Telemetry_Parse(&ctx, &d);
    CHECK(st == TEL_VALID, "parse status should be TEL_VALID");

    CHECK(d.protocol_version == 2U,        "protocol_version == 2");
    CHECK(d.sequence == 0U,                "sequence == 0");
    CHECK(d.motor_rpm == 1500U,            "motor_rpm == 1500");
    CHECK(d.motor_torque == -250,          "motor_torque == -250");
    CHECK(d.motor_error_flags == 5U,       "motor_error_flags == 5");
    CHECK(d.motor_data_valid == 1U,        "motor_data_valid == 1");
    CHECK(d.motor_timeout_active == 0U,    "motor_timeout_active == 0");
    CHECK(d.bms_cell_vmax_decimv == 37734U,"bms_cell_vmax_decimv == 37734");
    CHECK(d.bms_cell_vmin_decimv == 37422U,"bms_cell_vmin_decimv == 37422");
    CHECK(d.bms_temp_highest_c == 32,      "bms_temp_highest_c == 32");
    CHECK(d.bms_temp_lowest_c == 31,       "bms_temp_lowest_c == 31");
    CHECK(d.bms_system_state == 2U,        "bms_system_state == 2 (IDLE)");
    CHECK(d.bms_pack_voltage_deciv == 780U,"bms_pack_voltage_deciv == 780");
    CHECK(d.bms_current_centima == -181610L,"bms_current_centima == -181610");
    CHECK(d.bms_soc_hundredths == 6283U,   "bms_soc_hundredths == 6283");
    CHECK(d.bms_data_valid == 1U,          "bms_data_valid == 1");
    CHECK(d.timestamp_ms == 123456U,       "timestamp_ms == 123456");
    CHECK(d.speed_kmh_x10 == 425U,         "speed_kmh_x10 == 425");

    const TelStats_t *s = Telemetry_GetStats(&ctx);
    CHECK(s->good_packets == 1U, "good_packets == 1");
    CHECK(s->parse_fail == 0U,   "parse_fail == 0");
    CHECK(s->bad_version == 0U,  "bad_version == 0");
    CHECK(s->range_fail == 0U,   "range_fail == 0");
}

static void test_field_count_must_be_19(void)
{
    printf("test_field_count_must_be_19...\n");
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* Eski 15 alanlik (v1) satir artik 19 bekleyen parser'da reddedilmeli. */
    const char *old_15_field_line =
        "TEL,1,0,1500,-250,5,1,0,80,-120,32,360,3700,0,1";
    Feed_Line(&ctx, old_15_field_line, 1000U);

    CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "18/15-field line must not produce a frame");
    const TelStats_t *s = Telemetry_GetStats(&ctx);
    CHECK(s->parse_fail == 1U, "parse_fail should increment for wrong field count");
    CHECK(s->good_packets == 0U, "good_packets must stay 0");
}

static void test_bad_version_rejected(void)
{
    printf("test_bad_version_rejected...\n");
    TelCtx_t ctx;
    Telemetry_Init(&ctx);

    /* 19 alan ama ver=1 (eski protokol numarasi) -> bad_version */
    const char *line =
        "TEL,1,0,1500,-250,5,1,0,37734,37422,32,31,2,780,-181610,6283,1,123456,425";
    Feed_Line(&ctx, line, 1000U);

    CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "bad-version line must not produce a frame");
    const TelStats_t *s = Telemetry_GetStats(&ctx);
    CHECK(s->bad_version == 1U, "bad_version should increment");
    CHECK(s->good_packets == 0U, "good_packets must stay 0");
}

static void test_out_of_range_fields_rejected(void)
{
    printf("test_out_of_range_fields_rejected...\n");

    /* sysState=5 (gecerli araligin disinda: 1..4) */
    {
        TelCtx_t ctx;
        Telemetry_Init(&ctx);
        const char *line =
            "TEL,2,0,1500,-250,5,1,0,37734,37422,32,31,5,780,-181610,6283,1,123456,425";
        Feed_Line(&ctx, line, 1000U);
        CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "sysState=5 must be rejected");
        CHECK(Telemetry_GetStats(&ctx)->parse_fail == 1U, "sysState=5 -> parse_fail");
    }

    /* tempH=128 (gecerli aralik -128..127 disinda) */
    {
        TelCtx_t ctx;
        Telemetry_Init(&ctx);
        const char *line =
            "TEL,2,0,1500,-250,5,1,0,37734,37422,128,31,2,780,-181610,6283,1,123456,425";
        Feed_Line(&ctx, line, 1000U);
        CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "tempH=128 must be rejected");
        CHECK(Telemetry_GetStats(&ctx)->parse_fail == 1U, "tempH=128 -> parse_fail");
    }

    /* spd_x10=3001 (gecerli aralik 0..3000 disinda) */
    {
        TelCtx_t ctx;
        Telemetry_Init(&ctx);
        const char *line =
            "TEL,2,0,1500,-250,5,1,0,37734,37422,32,31,2,780,-181610,6283,1,123456,3001";
        Feed_Line(&ctx, line, 1000U);
        CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "spd_x10=3001 must be rejected");
        CHECK(Telemetry_GetStats(&ctx)->parse_fail == 1U, "spd_x10=3001 -> parse_fail");
    }

    /* rpm sanity ustu (TEL_RPM_MAX=20000 ama tip siniri 65535) -> range_fail */
    {
        TelCtx_t ctx;
        Telemetry_Init(&ctx);
        const char *line =
            "TEL,2,0,30000,-250,5,1,0,37734,37422,32,31,2,780,-181610,6283,1,123456,425";
        Feed_Line(&ctx, line, 1000U);
        CHECK(Telemetry_IsFrameReady(&ctx) == 0U, "rpm=30000 must be rejected");
        CHECK(Telemetry_GetStats(&ctx)->range_fail == 1U, "rpm=30000 -> range_fail (sanity)");
    }
}

int main(void)
{
    test_real_aks_vector();
    test_field_count_must_be_19();
    test_bad_version_rejected();
    test_out_of_range_fields_rejected();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
