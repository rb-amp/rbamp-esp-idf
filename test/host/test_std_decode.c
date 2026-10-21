/* Host unit test for the pure STD decode helpers (µWh/µvarh energy, delta
 * rules, starting current, voltage angle, phase sequence). No hardware.
 * Run: test/host/run.sh */
#include "rbamp_std_decode.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(u >> (8 * i));
}

static void fine_block(uint8_t *b, int64_t imp, int64_t exp, int64_t net, int64_t rimp, int64_t rexp)
{
    memset(b, 0xA5, RBAMPI_ENERGY_FINE_LEN);
    put_i64(b + (RBAMP_STD_ENERGY_E_ACT_IMP_UWH   - RBAMPI_ENERGY_FINE_OFF), imp);
    put_i64(b + (RBAMP_STD_ENERGY_E_ACT_EXP_UWH   - RBAMPI_ENERGY_FINE_OFF), exp);
    put_i64(b + (RBAMP_STD_ENERGY_E_ACT_NET_UWH   - RBAMPI_ENERGY_FINE_OFF), net);
    put_i64(b + (RBAMP_STD_ENERGY_E_REA_IMP_UVARH - RBAMPI_ENERGY_FINE_OFF), rimp);
    put_i64(b + (RBAMP_STD_ENERGY_E_REA_EXP_UVARH - RBAMPI_ENERGY_FINE_OFF), rexp);
}

static void test_layout(void)
{
    CHECK(RBAMPI_ENERGY_FINE_OFF == 0xD0);
    CHECK(RBAMPI_ENERGY_FINE_LEN == 40);
    CHECK(RBAMP_STD_GROUP_ENERGY_UWH_BASE == 0x0C80 && RBAMP_STD_GROUP_ENERGY_UWH_STRIDE == 0x10);
    CHECK(RBAMP_STD_REG_CFG_START_UA == 0x0084 && RBAMP_STD_REG_PHASE_SEQ == 0x0012);
    CHECK(RBAMP_STD_VOLTAGE_ANGLE == 0x0A && RBAMP_STD_CAP_ENERGY_UWH == 0x8000);
}

static void test_decode_fine(void)
{
    uint8_t b[RBAMPI_ENERGY_FINE_LEN];
    rbamp_std_energy_t e; memset(&e, 0, sizeof e);
    fine_block(b, 1234567, 7, 1234560, 55, 66);
    rbampi_energy_decode_fine(&e, b, true);
    CHECK(e.act_import_uwh == 1234567 && e.act_export_uwh == 7 && e.act_net_uwh == 1234560);
    CHECK(e.rea_import_uvarh == 55 && e.rea_export_uvarh == 66);
    CHECK(e.fine && e.fine_reactive);

    /* image 0x7A: bit15 set, reactive µvarh not yet published → derived from varh */
    memset(&e, 0, sizeof e);
    e.rea_import_varh = 3; e.rea_export_varh = 2;
    e.act_import_mwh = 999; /* must not be used: active is fine */
    rbampi_energy_decode_fine(&e, b, false);
    rbampi_energy_fill_coarse(&e, true, false, true);
    CHECK(e.fine && !e.fine_reactive);
    CHECK(e.act_import_uwh == 1234567);
    CHECK(e.rea_import_uvarh == 3000000 && e.rea_export_uvarh == 2000000);

    /* negative i64 round-trips (net can be negative) */
    fine_block(b, 0, 5, -5, 0, 0);
    rbampi_energy_decode_fine(&e, b, true);
    CHECK(e.act_net_uwh == -5 && e.act_export_uwh == 5);
}

static void test_fill_coarse(void)
{
    rbamp_std_energy_t e; memset(&e, 0, sizeof e);
    e.act_import_wh = 100; e.act_export_wh = 1; e.act_net_wh = 99;
    e.act_import_mwh = 100123; e.act_export_mwh = 1500; e.act_net_mwh = 98623;
    e.rea_import_varh = 4; e.rea_export_varh = 0;
    e.channels = 2; e.ch_import_mwh[0] = 60000; e.ch_export_mwh[0] = 1500; e.ch_import_mwh[1] = 40123;
    e.fine = true; e.fine_reactive = true;   /* stale flags must be cleared */
    rbampi_energy_fill_coarse(&e, false, false, true);
    CHECK(!e.fine && !e.fine_reactive);
    CHECK(e.act_import_uwh == 100123000 && e.act_export_uwh == 1500000 && e.act_net_uwh == 98623000);
    CHECK(e.rea_import_uvarh == 4000000 && e.rea_export_uvarh == 0);
    CHECK(e.ch_import_uwh[0] == 60000000 && e.ch_export_uwh[0] == 1500000 && e.ch_import_uwh[1] == 40123000);
    CHECK(e.ch_import_uwh[2] == 0);

    /* below map 0x36: no mWh — scale the Wh */
    memset(&e, 0, sizeof e);
    e.act_import_wh = 3; e.act_export_wh = 1; e.act_net_wh = 2; e.rea_import_varh = 1;
    rbampi_energy_fill_coarse(&e, false, false, false);
    CHECK(e.act_import_uwh == 3000000 && e.act_export_uwh == 1000000 && e.act_net_uwh == 2000000);
    CHECK(e.rea_import_uvarh == 1000000 && !e.fine);
}

static void test_group_fine(void)
{
    uint8_t g[3 * RBAMP_STD_GROUP_ENERGY_UWH_STRIDE];
    memset(g, 0, sizeof g);
    put_i64(g + 0 * 16 + RBAMP_STD_GROUP_ENERGY_UWH_E_IMP, 11);
    put_i64(g + 0 * 16 + RBAMP_STD_GROUP_ENERGY_UWH_E_EXP, 12);
    put_i64(g + 1 * 16 + RBAMP_STD_GROUP_ENERGY_UWH_E_IMP, 21);
    put_i64(g + 2 * 16 + RBAMP_STD_GROUP_ENERGY_UWH_E_EXP, 32);
    rbamp_std_energy_t e; memset(&e, 0, sizeof e);
    rbampi_energy_decode_group_fine(&e, g, 2);   /* only 2 channels populated */
    CHECK(e.ch_import_uwh[0] == 11 && e.ch_export_uwh[0] == 12 && e.ch_import_uwh[1] == 21);
    CHECK(e.ch_export_uwh[2] == 0);
}

static void test_delta(void)
{
    rbamp_std_energy_t a, b, d; bool restarted = false;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.generation = b.generation = 7;
    a.uptime_s = 1000; b.uptime_s = 1300;
    a.act_import_uwh = 10000; b.act_import_uwh = 12500;
    a.act_export_uwh = 0;     b.act_export_uwh = 300;
    a.act_net_uwh = 10000;    b.act_net_uwh = 12200;
    a.rea_import_uvarh = 5;   b.rea_import_uvarh = 9;
    a.rea_export_uvarh = 5;   b.rea_export_uvarh = 4;    /* went down → 0 */
    a.act_import_mwh = 10;    b.act_import_mwh = 12;
    a.fine = b.fine = true; a.fine_reactive = true; b.fine_reactive = false;
    a.channels = b.channels = 2;
    a.ch_import_uwh[0] = 100; b.ch_import_uwh[0] = 150;
    a.ch_export_uwh[1] = 50;  b.ch_export_uwh[1] = 40;   /* went down → 0 */
    CHECK(rbamp_std_energy_delta(&a, &b, &d, &restarted) == ESP_OK);
    CHECK(!restarted);
    CHECK(d.act_import_uwh == 2500 && d.act_export_uwh == 300 && d.act_net_uwh == 2200);
    CHECK(d.rea_import_uvarh == 4 && d.rea_export_uvarh == 0);
    CHECK(d.act_import_mwh == 2 && d.act_net_mwh == 2);
    CHECK(d.fine && !d.fine_reactive);
    CHECK(d.channels == 2 && d.ch_import_uwh[0] == 50 && d.ch_export_uwh[1] == 0);
    CHECK(d.generation == 7 && d.uptime_s == 1300);

    /* NET = IMP − EXP of the clamped parts, never the net counters' difference */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.act_import_uwh = 10; a.act_export_uwh = 0; a.act_net_uwh = 10;
    b.act_import_uwh = 9;  b.act_export_uwh = 5; b.act_net_uwh = 4;    /* import resumed below */
    CHECK(rbamp_std_energy_delta(&a, &b, &d, NULL) == ESP_OK);
    CHECK(d.act_import_uwh == 0 && d.act_export_uwh == 5 && d.act_net_uwh == -5);

    /* restart: uptime decreasing is reported, counters still differenced */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.uptime_s = 5000; b.uptime_s = 12; a.act_import_uwh = 100; b.act_import_uwh = 100;
    CHECK(rbamp_std_energy_delta(&a, &b, &d, &restarted) == ESP_OK && restarted && d.act_import_uwh == 0);

    /* generation change: no delta */
    b.generation = 1;
    CHECK(rbamp_std_energy_delta(&a, &b, &d, &restarted) == ESP_ERR_INVALID_STATE);

    /* channel count differs: no per-channel part */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.channels = 1; b.channels = 3; b.ch_import_uwh[0] = 9;
    CHECK(rbamp_std_energy_delta(&a, &b, &d, NULL) == ESP_OK && d.channels == 0 && d.ch_import_uwh[0] == 0);

    CHECK(rbamp_std_energy_delta(NULL, &b, &d, NULL) == ESP_ERR_INVALID_ARG);
}

static void test_start_ua(void)
{
    CHECK(rbampi_start_ua_valid(0));
    CHECK(rbampi_start_ua_valid(10));
    CHECK(rbampi_start_ua_valid(400));
    CHECK(rbampi_start_ua_valid(2540));
    CHECK(!rbampi_start_ua_valid(1));
    CHECK(!rbampi_start_ua_valid(5));
    CHECK(!rbampi_start_ua_valid(15));
    CHECK(!rbampi_start_ua_valid(2545));
    CHECK(!rbampi_start_ua_valid(2550));
    CHECK(!rbampi_start_ua_valid(0xFFFF));
}

static void test_angle(void)
{
    float deg = 1.0f;
    CHECK(rbampi_angle_decode(-12000, true, &deg) && fabsf(deg + 120.0f) < 1e-4f);
    CHECK(rbampi_angle_decode(12000, true, &deg) && fabsf(deg - 120.0f) < 1e-4f);
    CHECK(rbampi_angle_decode(0, true, &deg) && deg == 0.0f);
    CHECK(rbampi_angle_decode(18000, true, &deg) && fabsf(deg - 180.0f) < 1e-4f);
    deg = 1.0f;
    CHECK(!rbampi_angle_decode(RBAMPI_ANGLE_NONE, true, &deg) && deg == 0.0f);
    deg = 1.0f;
    CHECK(!rbampi_angle_decode(-12000, false, &deg) && deg == 0.0f);   /* image < 0x7A: byte reads 0, treated as n/a */
    /* raw 0x8000 arrives as the LE pair 00 80 */
    const uint8_t raw[2] = { 0x00, 0x80 };
    CHECK(rbampi_le_i16(raw) == RBAMPI_ANGLE_NONE);
}

static void test_phase_seq(void)
{
    CHECK(rbampi_phase_seq_decode(0) == RBAMP_STD_PHASE_SEQ_NA);
    CHECK(rbampi_phase_seq_decode(1) == RBAMP_STD_PHASE_SEQ_FORWARD);
    CHECK(rbampi_phase_seq_decode(2) == RBAMP_STD_PHASE_SEQ_REVERSE);
    CHECK(rbampi_phase_seq_decode(3) == RBAMP_STD_PHASE_SEQ_ABNORMAL);
    CHECK(rbampi_phase_seq_decode(7) == RBAMP_STD_PHASE_SEQ_NA);
    CHECK(rbampi_phase_seq_decode(0xFF) == RBAMP_STD_PHASE_SEQ_NA);
}

static void test_fw_gates(void)
{
    CHECK(RBAMPI_FW_ANGLE_MIN == 0x0001007Au);
    CHECK(RBAMPI_FW_UVARH_MIN == 0x0001007Bu && RBAMPI_FW_START_UA_MIN == 0x0001007Bu);
    CHECK(0x00010079u < RBAMPI_FW_ANGLE_MIN);      /* 0x79: nothing new */
    CHECK(0x0001007Au < RBAMPI_FW_UVARH_MIN);      /* 0x7A: angles/µWh yes, µvarh/start current no */
}

/* BASIC line: begin()-time variant policy for both constructors.
 * rbamp_new()               → pinned=false, channels start at the THREE_PHASE fallback (3).
 * rbamp_new_with_topology() → pinned=true,  channels = the caller's count. */
static void test_basic_variant(void)
{
    uint8_t ch; bool volt; rbamp_variant_t var;

    /* rbamp_new + UI1 (0x01): module decides → 1 channel + voltage. */
    ch = 3; volt = false; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x01, false, &ch, &volt, &var) == RBAMPI_VARIANT_APPLIED);
    CHECK(ch == 1 && volt && var == RBAMP_VARIANT_UI1);

    /* rbamp_new + I2 (0x05): 2 channels, no voltage. */
    ch = 3; volt = true; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x05, false, &ch, &volt, &var) == RBAMPI_VARIANT_APPLIED);
    CHECK(ch == 2 && !volt && var == RBAMP_VARIANT_I2);

    /* rbamp_new + UI7 (0x08): senior SKU, 7 channels. */
    ch = 3; volt = false; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x08, false, &ch, &volt, &var) == RBAMPI_VARIANT_APPLIED);
    CHECK(ch == 7 && volt && var == RBAMP_VARIANT_UI7);

    /* rbamp_new_with_topology(THREE_PHASE) + UI1: pin kept (3), WARN, variant/voltage from module. */
    ch = 3; volt = false; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x01, true, &ch, &volt, &var) == RBAMPI_VARIANT_KEPT_PIN);
    CHECK(ch == 3 && volt && var == RBAMP_VARIANT_UI1);

    /* rbamp_new_with_topology(SINGLE) + UI1: pin equals module → no warning. */
    ch = 1; volt = false; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x01, true, &ch, &volt, &var) == RBAMPI_VARIANT_APPLIED);
    CHECK(ch == 1 && volt && var == RBAMP_VARIANT_UI1);

    /* HW_VARIANT 0 (pre-v1.3 firmware): fallback — nothing touched, variant stays UNKNOWN. */
    ch = 3; volt = false; var = RBAMP_VARIANT_UNKNOWN;
    CHECK(rbampi_basic_variant_apply(0x00, false, &ch, &volt, &var) == RBAMPI_VARIANT_UNKNOWN);
    CHECK(ch == 3 && !volt && var == RBAMP_VARIANT_UNKNOWN);
    CHECK(rbampi_basic_variant_apply(0x00, true, &ch, &volt, &var) == RBAMPI_VARIANT_UNKNOWN);
    CHECK(ch == 3);

    /* Unknown codes (STD codes 9/10, 0xFF) are not BASIC shapes. */
    CHECK(!rbampi_basic_variant_shape(0x09, NULL, NULL));
    CHECK(!rbampi_basic_variant_shape(0x0A, NULL, NULL));
    CHECK(!rbampi_basic_variant_shape(0xFF, NULL, NULL));
    CHECK(rbampi_basic_variant_shape(0x03, &ch, &volt) && ch == 3 && volt);
    CHECK(rbampi_basic_variant_shape(0x06, &ch, &volt) && ch == 3 && !volt);
}

/* BASIC line: resume after master downtime — module window, from-boot
 * detection, first-consumed-snapshot accounting, stale handling. */
#define W200(c) ((uint32_t)((uint64_t)(c) * 200u))   /* latch_ms consistent with a 200-ms image */

static void test_basic_resume(void)
{
    rbampi_basic_window_t w;
    bool pending, restarted;

    /* Resume, no module restart: uptime 3 h, window 2 h → from_boot false,
     * energy = avg x window (2 h of 230 W = 460 Wh). */
    rbampi_basic_window_decode(2u * 3600u * 1000u / 200u, W200(2u * 3600u * 1000u / 200u), 3u * 3600u, &w);
    CHECK(w.window_ms == 7200000u && w.uptime_s == 10800u && !w.from_boot && !w.is_long);
    pending = true; restarted = true;
    CHECK(rbampi_basic_period_dt(&pending, 123u, &w, &restarted) == 7200000u);
    CHECK(!pending && !restarted);
    CHECK(fabs((double)230.0f * 7200000.0 / 3.6e6 - 460.0) < 1e-9);

    /* Later snapshot: master dt again, flags clear. */
    CHECK(rbampi_basic_period_dt(&pending, 60000u, &w, &restarted) == 60000u);
    CHECK(!pending && !restarted);

    /* Resume after a module restart: window ≈ uptime (within 1.5 s) → from_boot. */
    rbampi_basic_window_decode(600u * 5u, W200(600u * 5u), 600u, &w);           /* 600 s window, 600 s uptime */
    CHECK(w.window_ms == 600000u && w.from_boot);
    rbampi_basic_window_decode(599u * 5u, W200(599u * 5u), 600u, &w);           /* 599.0 s vs 600 s: inside tolerance */
    CHECK(w.from_boot);
    rbampi_basic_window_decode(598u * 5u, W200(598u * 5u), 600u, &w);           /* 598.0 s vs 600 s: outside */
    CHECK(!w.from_boot);
    rbampi_basic_window_decode(601u * 5u, W200(601u * 5u), 600u, &w);           /* window > uptime (should not happen): still from boot */
    CHECK(w.from_boot);
    pending = true; restarted = false;
    rbampi_basic_window_decode(600u * 5u, W200(600u * 5u), 600u, &w);
    CHECK(rbampi_basic_period_dt(&pending, 0u, &w, &restarted) == 600000u && restarted && !pending);

    /* Stale first read: the caller never reaches period_dt (PERIOD_VALID 0 →
     * return before consume), so pending stays set and the next valid read
     * still integrates the module window. */
    pending = true;
    rbampi_basic_window_decode(2000u, W200(2000u), 5000u, &w);              /* 400 s window */
    /* ... stale read: no call ... */
    CHECK(pending);
    CHECK(rbampi_basic_period_dt(&pending, 777u, &w, &restarted) == 400000u && !pending && !restarted);

    /* Long window flag: 6 h exactly is not long, one commit more is. */
    rbampi_basic_window_decode(6u * 3600u * 5u, W200(6u * 3600u * 5u), 0u, &w);
    CHECK(w.window_ms == 21600000u && !w.is_long);
    rbampi_basic_window_decode(6u * 3600u * 5u + 1u, W200(6u * 3600u * 5u + 1u), 0u, &w);
    CHECK(w.is_long);

    /* Uptime register absent (reads 0): never from boot, resume still integrates the window. */
    rbampi_basic_window_decode(3000u, W200(3000u), 0u, &w);
    CHECK(w.uptime_s == 0 && !w.from_boot && w.window_ms == 600000u);
    pending = true; restarted = true;
    CHECK(rbampi_basic_period_dt(&pending, 5u, &w, &restarted) == 600000u && !restarted);

    /* Commit period is inferred from the same snapshot (firmware: 100 ms since v1.2, 200 ms before).
     * hub measurement: 374 s real window, 3740 commits → 100 ms → 374 s (was reported 748 s). */
    rbampi_basic_window_decode(3740u, 374000u, 0u, &w);
    CHECK(w.commit_ms == 100 && w.window_ms == 374000u);
    rbampi_basic_window_decode(3740u, 280500u, 0u, &w);                 /* latch_ms short by 25 % → still 100 */
    CHECK(w.commit_ms == 100 && w.window_ms == 374000u);
    rbampi_basic_window_decode(1870u, 374000u, 0u, &w);                 /* 5 kHz image: 200 ms */
    CHECK(w.commit_ms == 200 && w.window_ms == 374000u);
    rbampi_basic_window_decode(1870u, 261800u, 0u, &w);                 /* 200-ms image, latch short by 30 % (140) → 200 */
    CHECK(w.commit_ms == 200);
    CHECK(rbampi_basic_commit_ms(1000u, 119999u) == 100 && rbampi_basic_commit_ms(1000u, 120000u) == 200);  /* boundary 120 ms */
    CHECK(rbampi_basic_commit_ms(1000u, 100000u) == 100 && rbampi_basic_commit_ms(1000u, 70000u) == 100);    /* 100-ms image, exact / 30 % short */
    CHECK(rbampi_basic_commit_ms(1000u, 200000u) == 200 && rbampi_basic_commit_ms(1000u, 130000u) == 200);   /* 200-ms image, exact / 35 % short */
    rbampi_basic_window_decode(0u, 374000u, 0u, &w);                    /* no commits: fallback 100, empty window */
    CHECK(w.commit_ms == 100 && w.window_ms == 0u);
    rbampi_basic_window_decode(3740u, 0u, 0u, &w);                      /* latch_ms 0: fallback 100 */
    CHECK(w.commit_ms == 100 && w.window_ms == 374000u);
    /* from_boot / long use the corrected window: 3740 commits ≈ 374 s uptime → from boot on a 100-ms image */
    rbampi_basic_window_decode(3740u, 374000u, 374u, &w);
    CHECK(w.from_boot);
    rbampi_basic_window_decode(3740u, 374000u, 748u, &w);               /* would have looked "from boot" at 200 ms */
    CHECK(!w.from_boot);
    rbampi_basic_window_decode(6u * 3600u * 10u, 6u * 3600u * 1000u, 0u, &w);
    CHECK(w.commit_ms == 100 && w.window_ms == 21600000u && !w.is_long);
    rbampi_basic_window_decode(6u * 3600u * 10u + 1u, 6u * 3600u * 1000u, 0u, &w);
    CHECK(w.is_long);

    /* Saturation: a commit count beyond 49.7 days saturates instead of wrapping. */
    rbampi_basic_window_decode(0xFFFFFFFFu, W200(0xFFFFFFFFu), 0u, &w);
    CHECK(w.window_ms == 0xFFFFFFFFu && w.is_long);
}

/* Sensor class / CT model write policy + rated-current table. */
static void test_sensor_class_guard(void)
{
    /* paired classes */
    CHECK(!rbampi_sensor_class_paired(0) && !rbampi_sensor_class_paired(1));
    CHECK(rbampi_sensor_class_paired(2) && rbampi_sensor_class_paired(3));

    /* class writes: unchanged = no-op (any class, incl. paired), paired → refused, else proceed */
    CHECK(rbampi_sensor_class_write_policy(1, 1) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_sensor_class_write_policy(2, 2) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_sensor_class_write_policy(3, 3) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_sensor_class_write_policy(2, 1) == RBAMPI_WRITE_PAIRED);
    CHECK(rbampi_sensor_class_write_policy(3, 1) == RBAMPI_WRITE_PAIRED);
    CHECK(rbampi_sensor_class_write_policy(0, 1) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_sensor_class_write_policy(1, 2) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_sensor_class_write_policy(0, 2) == RBAMPI_WRITE_PROCEED);

    /* model writes on class 1 (SCT-): same model = no-op (addendum), other model proceeds */
    CHECK(rbampi_ct_model_write_policy(1, 3, 3, false) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_ct_model_write_policy(1, 3, 6, false) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_ct_model_write_policy(0, 0, 3, false) == RBAMPI_WRITE_PROCEED);
    /* model writes on class 2/3 (CT-): same model = no-op (reconnect-safe), other → refused */
    CHECK(rbampi_ct_model_write_policy(2, 8, 8, false) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_ct_model_write_policy(2, 8, 9, false) == RBAMPI_WRITE_PAIRED);
    CHECK(rbampi_ct_model_write_policy(3, 1, 2, false) == RBAMPI_WRITE_PAIRED);
    /* unreadable mirror (0xFF) is never "same" */
    CHECK(rbampi_ct_model_write_policy(1, 0xFF, 3, false) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_ct_model_write_policy(2, 0xFF, 8, false) == RBAMPI_WRITE_PAIRED);
    /* force: always writes (service re-pairing), even same model / paired class */
    CHECK(rbampi_ct_model_write_policy(2, 8, 9, true) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_ct_model_write_policy(2, 8, 8, true) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_ct_model_write_policy(1, 3, 3, true) == RBAMPI_WRITE_PROCEED);

    /* limits / rated current come from the generated registry: rated = nominal_a
     * (the sensor's rating, not the SKU name: the "5 A" clamp is 4.6 A). */
    float nom = -1, fs = -1, pk = -1;
    CHECK(rbamp_sensor_model_limits(2, RBAMP_CT_CLAMP2000_5A, &nom, &fs, &pk));
    CHECK(fabsf(nom - 4.6f) < 1e-6f && fabsf(fs - 5.75f) < 1e-6f && fabsf(pk - 8.18f) < 1e-6f);
    CHECK(fabsf(rbamp_sensor_model_rated_a(2, RBAMP_CT_CLAMP2000_5A) - 4.6f) < 1e-6f);
    CHECK(fabsf(rbamp_sensor_model_rated_a(1, RBAMP_CT_SCT013_030) - 30.0f) < 1e-6f);
    CHECK(rbamp_sensor_model_limits(2, RBAMP_CT_TOROID_2000, &nom, &fs, &pk));   /* known model, unknown limits */
    CHECK(nom == 0.0f && fs == 0.0f && pk == 0.0f);
    CHECK(!rbamp_sensor_model_limits(9, 1, &nom, &fs, &pk) && nom == 0.0f);         /* unknown pair */
    CHECK(!rbamp_sensor_model_limits(1, 200, NULL, NULL, NULL));                    /* NULL outputs allowed */
    CHECK(rbamp_sensor_model_rated_a(9, 1) == 0.0f && rbamp_sensor_model_rated_a(1, 200) == 0.0f);
    for (unsigned i = 0; i < RBAMP_SENSOR_MODEL_COUNT; ++i) {
        const rbamp_sensor_model_info_t *m = &RBAMP_SENSOR_MODELS[i];
        CHECK(rbamp_sensor_model_limits(m->sensor_class, m->code, &nom, &fs, &pk));
        CHECK(nom == m->nominal_a && fs == m->fs_a && pk == m->peak_a);
        CHECK(rbamp_sensor_model_rated_a(m->sensor_class, m->code) == m->nominal_a);
        /* sanity of the registry itself: a known clip onset is below the peak
         * ceiling. (nominal vs fs is NOT ordered: SCT-013 clips below its rating
         * on this front end, the clamp line above it.) */
        if (m->nominal_a > 0) CHECK(m->fs_a > 0 && m->fs_a < m->peak_a);
    }
}

/* configure_channels: whole-request decision before any write. */
static void test_configure_policy(void)
{
    uint8_t cur[3], req[3];
    /* SCT- module, same class, same models → nothing to do */
    cur[0]=3; cur[1]=3; cur[2]=6; req[0]=3; req[1]=3; req[2]=6;
    CHECK(rbampi_configure_policy(1, 1, cur, req, 3) == RBAMPI_WRITE_NOOP);
    /* zero = unconfigured channel, ignored even if the module holds something else */
    req[2]=0;
    CHECK(rbampi_configure_policy(1, 1, cur, req, 3) == RBAMPI_WRITE_NOOP);
    /* one model differs → proceed */
    req[2]=4;
    CHECK(rbampi_configure_policy(1, 1, cur, req, 3) == RBAMPI_WRITE_PROCEED);
    /* class changes on an SCT- module → proceed even with equal models (fw clears them) */
    req[2]=6;
    CHECK(rbampi_configure_policy(1, 2, cur, req, 3) == RBAMPI_WRITE_PROCEED);
    CHECK(rbampi_configure_policy(0, 1, cur, req, 3) == RBAMPI_WRITE_PROCEED);
    /* paired module (class 2): all equal → no-op; any model change → refused; class change → refused */
    cur[0]=8; cur[1]=8; cur[2]=8; req[0]=8; req[1]=8; req[2]=8;
    CHECK(rbampi_configure_policy(2, 2, cur, req, 3) == RBAMPI_WRITE_NOOP);
    req[1]=9;
    CHECK(rbampi_configure_policy(2, 2, cur, req, 3) == RBAMPI_WRITE_PAIRED);
    req[1]=8;
    CHECK(rbampi_configure_policy(2, 1, cur, req, 3) == RBAMPI_WRITE_PAIRED);
    CHECK(rbampi_configure_policy(3, 3, cur, req, 3) == RBAMPI_WRITE_NOOP);
    req[0]=0; req[1]=0; req[2]=0;
    CHECK(rbampi_configure_policy(3, 3, cur, req, 3) == RBAMPI_WRITE_NOOP);   /* no models requested */
    /* unreadable mirror on a paired module with a requested model → refused (never "same") */
    cur[0]=0xFF; req[0]=8;
    CHECK(rbampi_configure_policy(2, 2, cur, req, 3) == RBAMPI_WRITE_PAIRED);
    /* n = 0 → only the class matters */
    CHECK(rbampi_configure_policy(1, 1, cur, req, 0) == RBAMPI_WRITE_NOOP);
    CHECK(rbampi_configure_policy(2, 1, cur, req, 0) == RBAMPI_WRITE_PAIRED);
}

/* BASIC digest: layout for masks 0x07 / 0x0F at 1 and 3 channels, decode,
 * re-arm once per RESET episode, firmware names. */
static void test_digest(void)
{
    rbampi_digest_layout_t L;
    /* 0x07 (I+U+P), 1 ch: [st][seq][I0 lo hi][U lo hi][P0 lo hi] = 8 */
    CHECK(rbampi_digest_layout(0x07, 1, &L) && L.len == 8 && L.off_i == 2 && L.off_u == 4 && L.off_p == 6 && L.off_pf == 0);
    /* 0x07, 3 ch: 2 + 6 + 2 + 6 = 16 */
    CHECK(rbampi_digest_layout(0x07, 3, &L) && L.len == 16 && L.off_i == 2 && L.off_u == 8 && L.off_p == 10 && L.off_pf == 0);
    /* 0x0F, 1 ch: 9; 3 ch: 19 (<= block size 22) */
    CHECK(rbampi_digest_layout(0x0F, 1, &L) && L.len == 9 && L.off_pf == 8);
    CHECK(rbampi_digest_layout(0x0F, 3, &L) && L.len == 19 && L.off_i == 2 && L.off_u == 8 && L.off_p == 10 && L.off_pf == 16);
    CHECK(L.len <= RBAMP_V2_REG_DIGEST_SIZE);
    /* I+P only (current-only module default), 2 ch: 2 + 4 + 4 = 10, no U slot */
    CHECK(rbampi_digest_layout(0x05, 2, &L) && L.len == 10 && L.off_u == 0 && L.off_p == 6);
    /* refusals: empty mask, unknown bit, 0 or 4 channels */
    CHECK(!rbampi_digest_layout(0x00, 1, &L) && !rbampi_digest_layout(0x10, 1, &L));
    CHECK(!rbampi_digest_layout(0x07, 0, &L) && !rbampi_digest_layout(0x07, 4, &L));

    /* decode 0x0F @ 3 ch: I = 12.34 / 0.05 / 150.00 A, U = 230.1 V, P = -5 / 0 / 1234 W, PF = 97 / 0 / 100 */
    uint8_t b[19] = {0};
    b[0] = 0x05; b[1] = 0xFE;
    b[2] = 0xD2; b[3] = 0x04;  b[4] = 0x05; b[5] = 0x00;  b[6] = 0x98; b[7] = 0x3A;
    b[8] = 0xFD; b[9] = 0x08;
    b[10] = 0xFB; b[11] = 0xFF;  b[12] = 0x00; b[13] = 0x00;  b[14] = 0xD2; b[15] = 0x04;
    b[16] = 97; b[17] = 0; b[18] = 100;
    rbamp_digest_t d;
    rbampi_digest_decode(b, 0x0F, 3, &d);
    CHECK(d.status == 0x05 && d.seq == 0xFE && d.n_ch == 3 && d.fields == 0x0F);
    CHECK(fabsf(d.i_a[0] - 12.34f) < 1e-4f && fabsf(d.i_a[1] - 0.05f) < 1e-4f && fabsf(d.i_a[2] - 150.0f) < 1e-3f);
    CHECK(fabsf(d.u_v - 230.1f) < 1e-3f);
    CHECK(d.p_w[0] == -5 && d.p_w[1] == 0 && d.p_w[2] == 1234);
    CHECK(fabsf(d.pf[0] - 0.97f) < 1e-4f && d.pf[1] == 0.0f && fabsf(d.pf[2] - 1.0f) < 1e-4f);
    /* decode 0x07 @ 1 ch: fields not requested stay 0 */
    uint8_t c[8] = { 0x01, 7, 0x10, 0x27, 0xFE, 0x08, 0x2C, 0x01 };   /* I 100.00 A, U 230.2 V, P 300 W */
    rbampi_digest_decode(c, 0x07, 1, &d);
    CHECK(d.seq == 7 && d.n_ch == 1 && fabsf(d.i_a[0] - 100.0f) < 1e-3f && fabsf(d.u_v - 230.2f) < 1e-3f && d.p_w[0] == 300);
    CHECK(d.pf[0] == 0.0f && d.i_a[1] == 0.0f && d.p_w[2] == 0);
    /* invalid layout → zeroed output */
    rbampi_digest_decode(c, 0x00, 1, &d);
    CHECK(d.n_ch == 0 && d.fields == 0);

    /* re-arm after restart: once per RESET episode; nothing pending → episode closed */
    bool seen = false;
    CHECK(!rbampi_digest_needs_rearm(&seen, false, 0));                                       /* quiet */
    CHECK(rbampi_digest_needs_rearm(&seen, true, RBAMP_V2_EVENT_RESET_OCCURRED) && seen);     /* restart → re-arm */
    CHECK(!rbampi_digest_needs_rearm(&seen, true, RBAMP_V2_EVENT_RESET_OCCURRED) && seen);    /* same episode: no */
    CHECK(!rbampi_digest_needs_rearm(&seen, true, 0x01) && !seen);                            /* other event only: episode closed */
    CHECK(rbampi_digest_needs_rearm(&seen, true, RBAMP_V2_EVENT_RESET_OCCURRED | 0x01));       /* new restart → re-arm */
    CHECK(!rbampi_digest_needs_rearm(&seen, false, RBAMP_V2_EVENT_RESET_OCCURRED) && !seen);  /* status says nothing pending */

    /* firmware names */
    CHECK(strcmp(rbamp_basic_fw_name(0x0B), "1.4.19") == 0 && strcmp(rbamp_basic_fw_name(0x0A), "1.4.18") == 0);
    CHECK(strcmp(rbamp_basic_fw_name(0x07), "1.4.15-1.4.17") == 0 && strcmp(rbamp_basic_fw_name(0x05), "1.4.4-1.4.14") == 0);
    CHECK(strcmp(rbamp_basic_fw_name(0x04), "1.2-1.4.3") == 0);
    CHECK(strcmp(rbamp_basic_fw_name(0x03), "<1.2") == 0 && strcmp(rbamp_basic_fw_name(0x00), "<1.2") == 0);
    CHECK(rbamp_basic_fw_name(0x06) == NULL && rbamp_basic_fw_name(0x08) == NULL && rbamp_basic_fw_name(0x09) == NULL);
    CHECK(rbamp_basic_fw_name(0x0C) == NULL && rbamp_basic_fw_name(0xFF) == NULL);
}

static void test_sct_rating(void)
{
    /* libs/spec/SCT_RATING_SWITCH.md, Tests */
    bool nr; float s;
    CHECK(rbampi_sct_decide(0x0008, 1, 5, 0.6414f, false, &nr, &s) == RBAMP_SCT_CORRECTED && !nr && fabsf(s - 3.276f) < 1e-3f);
    CHECK(rbampi_sct_decide(0x0008, 1, 3, 0.6414f, false, &nr, &s) == RBAMP_SCT_NONE && s == 1.0f);
    CHECK(rbampi_sct_decide(0x0008, 1, 5, rbampi_sct_preset_gain(5), false, &nr, &s) == RBAMP_SCT_PENDING && nr && s == 1.0f);
    CHECK(rbampi_sct_decide(0x0008, 1, 5, rbampi_sct_preset_gain(5), true, &nr, &s) == RBAMP_SCT_UNCALIBRATED && !nr && s == 1.0f);
    CHECK(rbampi_sct_decide(0x0008, 1, 5, 0.6349f * 1.08f, false, &nr, &s) == RBAMP_SCT_GAIN_UNEXPECTED && s == 1.0f);
    CHECK(rbampi_sct_decide(RBAMPI_CAP_SCT_RATING_SELF, 1, 5, 0.6414f, false, &nr, &s) == RBAMP_SCT_NONE && s == 1.0f);   /* the module corrects itself */
    CHECK(rbampi_sct_decide(0x0FFF, 1, 5, 0.6414f, false, &nr, &s) == RBAMP_SCT_CORRECTED);                                /* every other bit set: still corrected */
    CHECK(RBAMPI_CAP_SCT_RATING_SELF == 0x1000u);
    /* beyond the spec list */
    CHECK(rbampi_sct_decide(0x0008, 2, 5, 0.6414f, false, &nr, &s) == RBAMP_SCT_NONE && s == 1.0f);          /* factory pair */
    CHECK(rbampi_sct_decide(0x0008, 1, 0, 0.6414f, false, &nr, &s) == RBAMP_SCT_NONE && s == 1.0f);          /* no model bound */
    CHECK(rbampi_sct_decide(0x0008, 1, 9, 0.6414f, false, &nr, &s) == RBAMP_SCT_UNKNOWN_MODEL && s == 1.0f);
    CHECK(rbampi_sct_decide(0x0008, 1, 1, 0.6414f, false, &nr, &s) == RBAMP_SCT_CORRECTED && fabsf(s - 0.1058f / 0.6349f) < 1e-4f);
    CHECK(rbampi_sct_decide(0x0008, 1, 4, 0.6349f * 0.951f, false, &nr, &s) == RBAMP_SCT_CORRECTED);        /* -4.9 % is still inside */
    CHECK(rbampi_sct_decide(0x0008, 1, 4, 0.6349f * 0.94f, false, &nr, &s) == RBAMP_SCT_GAIN_UNEXPECTED);
    CHECK(rbampi_sct_decide(0x0008, 1, 4, 0.0f, false, &nr, &s) == RBAMP_SCT_GAIN_UNEXPECTED);
    CHECK(rbampi_sct_decide(0x0008, 1, 4, NAN, false, &nr, &s) == RBAMP_SCT_GAIN_UNEXPECTED);
    CHECK(rbampi_sct_decide(0x0000, 1, 7, 0.6414f, false, &nr, &s) == RBAMP_SCT_CORRECTED && fabsf(s - 1.2698f / 0.6349f) < 1e-4f);
    /* the only model with a rating correction is the -100 */
    for (uint8_t m = 1; m <= 7; ++m) CHECK(rbampi_sct_rating_corr(m) == (m == 5 ? 0.962f : 1.0f));
    CHECK(rbampi_sct_preset_gain(0) == 0.0f && rbampi_sct_preset_gain(8) == 0.0f && rbampi_sct_preset_gain(3) == 0.6349f);
}

int main(void)
{
    test_digest();
    test_configure_policy();
    test_sensor_class_guard();
    test_basic_resume();
    test_basic_variant();
    test_layout();
    test_decode_fine();
    test_fill_coarse();
    test_group_fine();
    test_delta();
    test_start_ua();
    test_angle();
    test_phase_seq();
    test_fw_gates();
    test_sct_rating();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
