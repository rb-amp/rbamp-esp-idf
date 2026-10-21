/* End-to-end host simulation of the SCT-013 rating switch
 * (libs/spec/SCT_RATING_SWITCH.md) through rbamp_begin, the reads, the
 * digest, a model switch and the restart paths. The fake module behaves like
 * BASIC fw 1.4.19: after CMD_RESET it applies the factory (030) gain whatever
 * model is bound; right after CMD_SET_CT_MODEL it applies the raw preset. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "rbamp.h"
#include "rbamp_std_decode.h"
#include "sim_module.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabsf((a) - (b)) <= (tol))

static const float G_CAL = 0.6414f;   /* factory 030 gain of the simulated unit (trim +1.0 %) */
static unsigned resets;
static bool     uncalibrated_unit;    /* reset leaves the raw preset in place */

static void fw_on_command(uint8_t op)
{
    if (op == RBAMP_CMD_RESET) {
        resets++;
        if (!uncalibrated_unit) sim_set_f32(RBAMP_REG_V03_I0_GAIN, G_CAL);
        sim_regs[RBAMP_V2_REG_EVENT_FLAGS] |= RBAMP_V2_EVENT_RESET_OCCURRED;
    } else if (op == RBAMP_CMD_SET_CT_MODEL_CH0) {
        sim_regs[RBAMP_V2_REG_CT_MODEL_CH0] = sim_regs[RBAMP_REG_CT_MODEL];
        sim_set_f32(RBAMP_REG_V03_I0_GAIN, rbampi_sct_preset_gain(sim_regs[RBAMP_REG_CT_MODEL]));   /* raw preset, trim lost */
    }
}

/* A UI1 on fw 1.4.19 in the flash state: class 1, model X bound, G_cal applied. */
static void image_ui1(uint16_t cap, uint8_t cls, uint8_t model, float gain)
{
    sim_reset_image();
    sim_on_command = fw_on_command;
    resets = 0;
    sim_regs[RBAMP_REG_STATUS]        = 0x01;
    sim_regs[RBAMP_REG_VERSION]       = 0x0B;
    sim_regs[RBAMP_V2_REG_HW_VARIANT] = 1;                      /* UI1 */
    sim_set_u16(RBAMP_V2_REG_CAPABILITY, (uint16_t)(RBAMP_V2_CAP_DIGEST | cap));
    sim_regs[RBAMP_REG_SENSOR_CLASS]  = cls;
    sim_regs[RBAMP_V2_REG_CT_MODEL_CH0] = model;
    sim_set_f32(RBAMP_REG_V03_I0_GAIN, gain);
    for (int i = 0; i < 12; ++i) sim_regs[RBAMP_V2_REG_UID + i] = (uint8_t)(0x10 + i);
    sim_set_f32(RBAMP_REG_V03_U_RMS, 230.0f);
    sim_set_f32(RBAMP_REG_V03_I0_RMS, 1.0f);
    sim_set_f32(RBAMP_REG_V03_I0_PEAK, 1.5f);
    sim_set_f32(RBAMP_REG_V03_P0_REAL, 100.0f);
    sim_set_f32(RBAMP_REG_V03_PF0, 0.9f);
    sim_set_f32(RBAMP_REG_V03_PERIOD_AVG_P_F0, 50.0f);
    sim_set_f32(RBAMP_REG_V03_PERIOD_MAX_P_F0, 120.0f);
    sim_regs[RBAMP_REG_V03_PERIOD_VALID] = 1;
    sim_regs[RBAMP_REG_V03_PERIOD_COMMIT_CNT] = 10;      /* 10 commits of 100 ms */
    sim_regs[RBAMP_REG_V03_PERIOD_LATCH_MS] = 0xE8;      /* 1000 ms */
    sim_regs[RBAMP_REG_V03_PERIOD_LATCH_MS + 1] = 0x03;
    /* digest, mask I+U+P: [status][seq][I 1.00 A][U 230.0 V][P 10 W] */
    sim_regs[RBAMP_V2_REG_DIGEST + 1] = 7;
    sim_set_u16(RBAMP_V2_REG_DIGEST + 2, 100);
    sim_set_u16(RBAMP_V2_REG_DIGEST + 4, 2300);
    sim_set_u16(RBAMP_V2_REG_DIGEST + 6, 10);
}

static rbamp_handle_t new_begun(void)
{
    rbamp_handle_t dev = NULL;
    esp_err_t err = rbamp_new((i2c_master_bus_handle_t)(uintptr_t)0x10, 0x50, &dev);
    CHECK(err == ESP_OK, "new: %d", err);
    err = rbamp_begin(dev);
    CHECK(err == ESP_OK, "begin: %d", err);
    return dev;
}

static void expect_scaled(rbamp_handle_t dev, float s, const char *why)
{
    float v = 0;
    CHECK(rbamp_read_current(dev, 0, &v) == ESP_OK && NEAR(v, 1.0f * s, 2e-3f), "%s: current %g (want %g)", why, v, s);
    CHECK(rbamp_read_current_peak(dev, 0, &v) == ESP_OK && NEAR(v, 1.5f * s, 3e-3f), "%s: peak %g", why, v);
    CHECK(rbamp_read_power(dev, 0, &v) == ESP_OK && NEAR(v, 100.0f * s, 0.2f), "%s: power %g (want %g)", why, v, 100 * s);
    CHECK(rbamp_read_power_factor(dev, 0, &v) == ESP_OK && NEAR(v, 0.9f, 1e-6f), "%s: pf %g must not scale", why, v);
    CHECK(rbamp_read_voltage(dev, 0, &v) == ESP_OK && NEAR(v, 230.0f, 1e-3f), "%s: voltage %g must not scale", why, v);
    CHECK(rbamp_read_period_avg_power(dev, 0, &v) == ESP_OK && NEAR(v, 50.0f * s, 0.1f), "%s: avg_p %g", why, v);
    CHECK(rbamp_read_period_max_power(dev, &v) == ESP_OK && NEAR(v, 120.0f * s, 0.3f), "%s: max_p %g", why, v);
    rbamp_digest_t d;
    esp_err_t err = rbamp_read_digest(dev, &d);
    CHECK(err == ESP_OK, "%s: digest %d", why, err);
    CHECK(NEAR(d.i_a[0], 1.0f * s, 2e-3f), "%s: digest I %g", why, d.i_a[0]);
    CHECK(d.p_w[0] == (int16_t)(10.0f * s), "%s: digest P %d (want %d)", why, d.p_w[0], (int)(10.0f * s));
    CHECK(NEAR(d.u_v, 230.0f, 1e-3f), "%s: digest U %g must not scale", why, d.u_v);
}

int main(void)
{
    const float S100 = 2.1622f * 0.962f / 0.6349f;   /* 3.276 per the spec */
    rbamp_sct_correction_t c;

    /* 1. Flash state, model 5 (-100), G_cal applied: corrected x3.276, no reset. */
    image_ui1(0, 1, 5, G_CAL);
    rbamp_handle_t dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_CORRECTED && NEAR(c.scale, S100, 1e-3f) && c.model == 5,
          "flash: status %d scale %g", c.status, c.scale);
    CHECK(resets == 0, "flash: %u resets", resets);
    expect_scaled(dev, S100, "flash -100");
    /* thresholds in true amps land in the module's scale */
    CHECK(rbamp_set_event_thresholds(dev, 32.76f, 3276.0f) == ESP_OK, "thresholds");
    CHECK(sim_get_u16(RBAMP_V2_REG_THRESH_I_HI) == 1000, "threshold I raw %u (want 1000)", sim_get_u16(RBAMP_V2_REG_THRESH_I_HI));
    CHECK(sim_get_u16(RBAMP_V2_REG_THRESH_P_HI) == 1000, "threshold P raw %u (want 1000)", sim_get_u16(RBAMP_V2_REG_THRESH_P_HI));
    /* period snapshot: avg_p and the energy window carry the scale */
    rbamp_period_snapshot_t ps;
    CHECK(rbamp_read_period_snapshot(dev, &ps, 0, false) == ESP_OK && ps.valid, "snapshot");
    CHECK(NEAR(ps.avg_p[0], 50.0f * S100, 0.1f) && NEAR(ps.avg_p_total, 50.0f * S100, 0.1f), "snapshot avg_p %g", ps.avg_p[0]);
    /* a detected restart (RESET acknowledged) re-reads and keeps the scale */
    CHECK(rbamp_clear_event_flags(dev, RBAMP_V2_EVENT_RESET_OCCURRED) == ESP_OK, "clear");
    expect_scaled(dev, S100, "after ack");
    rbamp_del(dev);

    /* 2. Model 3: no correction. */
    image_ui1(0, 1, 3, G_CAL);
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_NONE && c.scale == 1.0f, "030: status %d", c.status);
    expect_scaled(dev, 1.0f, "030");

    /* 2b. The user switches to -100 on this handle: bind, save, one restart, corrected. */
    CHECK(rbamp_set_ct_model_ch(dev, 0, 5) == ESP_OK, "set_ct_model_ch 5");
    CHECK(resets == 1, "switch: %u resets (want 1)", resets);
    CHECK(sim_regs[RBAMP_V2_REG_CT_MODEL_CH0] == 5, "switch: model %u", sim_regs[RBAMP_V2_REG_CT_MODEL_CH0]);
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_CORRECTED && NEAR(c.scale, S100, 1e-3f), "switch: status %d scale %g", c.status, c.scale);
    expect_scaled(dev, S100, "after switch");
    CHECK(rbamp_set_ct_model_ch(dev, 0, 5) == ESP_OK && resets == 1, "same model again: no write, no restart (%u)", resets);
    rbamp_del(dev);

    /* 3. Same-boot state found at begin (someone bound -100 without a restart): one CMD_RESET, then corrected. */
    image_ui1(0, 1, 5, rbampi_sct_preset_gain(5));
    dev = new_begun();
    CHECK(resets == 1, "same-boot: %u resets (want 1)", resets);
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_CORRECTED && NEAR(c.scale, S100, 1e-3f), "same-boot: status %d", c.status);
    CHECK((sim_regs[RBAMP_V2_REG_EVENT_FLAGS] & RBAMP_V2_EVENT_RESET_OCCURRED) != 0, "sim keeps the flag; library wrote the ack");
    rbamp_del(dev);

    /* 4. Uncalibrated unit: still the raw preset after the reset -> s = 1, exactly one reset. */
    uncalibrated_unit = true;
    image_ui1(0, 1, 5, rbampi_sct_preset_gain(5));
    dev = new_begun();
    CHECK(resets == 1, "uncal: %u resets (want 1)", resets);
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_UNCALIBRATED && c.scale == 1.0f, "uncal: status %d", c.status);
    expect_scaled(dev, 1.0f, "uncal");
    rbamp_del(dev);
    uncalibrated_unit = false;

    /* 5. Gain 8 % off the 030 preset: warning, s = 1. */
    image_ui1(0, 1, 5, 0.6349f * 1.08f);
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_GAIN_UNEXPECTED && c.scale == 1.0f, "off: status %d", c.status);
    CHECK(resets == 0, "off: %u resets", resets);
    rbamp_del(dev);

    /* 6. CAP_SCT_RATING_SELF set (fw 1.4.20+): s = 1, plain model write without a restart. */
    image_ui1(RBAMPI_CAP_SCT_RATING_SELF, 1, 5, G_CAL);
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_NONE && c.scale == 1.0f, "cap: status %d", c.status);
    CHECK(rbamp_set_ct_model_ch(dev, 0, 4) == ESP_OK && resets == 0, "cap: model write restarted the module (%u)", resets);
    expect_scaled(dev, 1.0f, "cap");
    rbamp_del(dev);
    /* 6b. VERSION 0x0C with the bit clear: still corrected — VERSION is not a gate. */
    image_ui1(0, 1, 5, G_CAL);
    sim_regs[RBAMP_REG_VERSION] = 0x0C;
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_CORRECTED, "0x0C without the bit: status %d", c.status);
    rbamp_del(dev);

    /* 7. Class 2 (factory pair) with a -100 model: untouched. */
    image_ui1(0, 2, 5, G_CAL);
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_NONE && c.scale == 1.0f, "class2: status %d", c.status);
    expect_scaled(dev, 1.0f, "class2");
    rbamp_del(dev);

    /* 8. Unknown model code. */
    image_ui1(0, 1, 9, G_CAL);
    dev = new_begun();
    CHECK(rbamp_sct_correction(dev, 0, &c) == ESP_OK && c.status == RBAMP_SCT_UNKNOWN_MODEL && c.scale == 1.0f, "unknown: status %d", c.status);
    rbamp_del(dev);

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
