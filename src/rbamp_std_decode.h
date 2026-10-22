/* Private: pure STD-line decode helpers shared by rbamp.c and the host tests
 * (test/host). No bus access, no IDF services — only rbamp.h types. */
#pragma once

#include "rbamp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First image versions that publish the new leaves (FW_VER 0x0114). */
#define RBAMPI_FW_ANGLE_MIN     0x0001007Au   /* VOLTAGE.ANGLE, PHASE_SEQ, SAVE_CAL cal_ok marker */
#define RBAMPI_FW_UVARH_MIN     0x0001007Bu   /* ENERGY +0xE8/+0xF0 reactive µvarh */
#define RBAMPI_FW_START_UA_MIN  0x0001007Bu   /* CFG_START_UA */

/* ENERGY +0xD0..+0xF7: active µWh (imp/exp/net) + reactive µvarh (imp/exp) — one 40-byte burst. */
#define RBAMPI_ENERGY_FINE_OFF  RBAMP_STD_ENERGY_E_ACT_IMP_UWH
#define RBAMPI_ENERGY_FINE_LEN  (RBAMP_STD_ENERGY_E_REA_EXP_UVARH + 8u - RBAMP_STD_ENERGY_E_ACT_IMP_UWH)

#define RBAMPI_START_UA_MAX     2540u
#define RBAMPI_START_UA_STEP    10u
#define RBAMPI_ANGLE_NONE       ((int16_t)0x8000)

static inline uint16_t rbampi_le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline int16_t rbampi_le_i16(const uint8_t *p) { return (int16_t)rbampi_le_u16(p); }
static inline uint32_t rbampi_le_u32(const uint8_t *p)
{
    return  (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rbampi_le_i32(const uint8_t *p) { return (int32_t)rbampi_le_u32(p); }
static inline int64_t rbampi_le_i64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

/* Decode the 40-byte fine burst into the µ fields. reactive_fine = false
 * leaves the µvarh fields for rbampi_energy_fill_coarse (image < 0x7B). */
void rbampi_energy_decode_fine(rbamp_std_energy_t *e, const uint8_t *b, bool reactive_fine);

/* Decode n GROUP_ENERGY_UWH entries (stride RBAMP_STD_GROUP_ENERGY_UWH_STRIDE). */
void rbampi_energy_decode_group_fine(rbamp_std_energy_t *e, const uint8_t *g, uint8_t n);

/* Derive every µ field the module did not publish from the coarse counters
 * (mWh·1000 when the map has mWh, else Wh·1e6; varh·1e6) and set the flags. */
void rbampi_energy_fill_coarse(rbamp_std_energy_t *e, bool active_fine, bool reactive_fine, bool have_mwh);

/* 0 = off, else 10..2540 µA in 10 µA steps. */
bool rbampi_start_ua_valid(uint16_t ua);

/* VOLTAGE.ANGLE raw → degrees. false when the image has no angles or raw = 0x8000. */
bool rbampi_angle_decode(int16_t raw, bool fw_has_angle, float *deg);

/* PHASE_SEQ raw byte → enum (unknown codes read as not available). */
rbamp_std_phase_seq_t rbampi_phase_seq_decode(uint8_t raw);

/* BASIC line: HW_VARIANT (0x55) byte → channel count + voltage presence.
 * Returns false for 0 (pre-v1.3 firmware) or an unknown code; outputs untouched. */
bool rbampi_basic_variant_shape(uint8_t hw, uint8_t *channels, bool *has_voltage);

typedef enum {
    RBAMPI_VARIANT_UNKNOWN  = 0,  /* byte not a known SKU: keep the constructor fallback */
    RBAMPI_VARIANT_APPLIED  = 1,  /* module decided (not pinned, or pin equals the module) */
    RBAMPI_VARIANT_KEPT_PIN = 2,  /* caller pinned a different count: pin kept, warn */
} rbampi_variant_result_t;

/* begin()-time policy shared by both constructors: @p pinned is true only for
 * rbamp_new_with_topology. Variant and voltage presence always follow the
 * module when the byte is known; the channel count follows the module unless
 * pinned. */
/* BASIC line: the module's own view of the latched period.
 * PERIOD_COMMIT_CNT (0xBE) counts commits on the module's measurement
 * clock — one per 100 ms on current images, per 200 ms on pre-v1.2 images.
 * The period is inferred from the same snapshot from latch_ms / commit_cnt:
 * below 120 ms per commit → 100, else 200 (latch_ms only reads short, so the
 * boundary sits nearer 100: a 200-ms image 30 % short still reads 140);
 * 100 when either is 0. UPTIME_S (0x46) is seconds
 * since the module booted (0 on firmware that does not publish it). The old
 * line below this comment is superseded by that rule:
 * clock; UPTIME_S (0x46) is seconds since the module booted (0 on firmware
 * that does not publish it). */
#define RBAMPI_BASIC_COMMIT_MS_FAST   100u   /* v1.2+ images */
#define RBAMPI_BASIC_COMMIT_MS_SLOW   200u   /* pre-v1.2 images */
#define RBAMPI_BASIC_FROM_BOOT_TOL_MS 1500u
#define RBAMPI_BASIC_WINDOW_LONG_MS   (6u * 3600u * 1000u)

typedef struct {
    uint32_t window_ms;   /* commit_cnt * commit_ms, saturated */
    uint16_t commit_ms;   /* 100 or 200, inferred */
    uint32_t uptime_s;    /* as read; 0 = not available */
    bool     from_boot;   /* window_ms >= uptime*1000 - 1500 (and uptime known) */
    bool     is_long;     /* window_ms > 6 h */
} rbampi_basic_window_t;

uint16_t rbampi_basic_commit_ms(uint32_t commit_cnt, uint32_t latch_ms);
void rbampi_basic_window_decode(uint32_t commit_cnt, uint32_t latch_ms, uint32_t uptime_s,
                                rbampi_basic_window_t *w);

/* Energy dt for a CONSUMED (valid) BASIC snapshot. The first consumed snapshot
 * after rbamp_begin_resume integrates over the module's own window and clears
 * *resume_pending; every later one uses the master's dt. Stale snapshots must
 * not call this (the pending state and the anchor are untouched). */
uint32_t rbampi_basic_period_dt(bool *resume_pending, uint32_t master_dt_ms,
                                const rbampi_basic_window_t *w, bool *module_restarted);

/* Sensor class / CT model write policy (both lines). Classes 2 (WIRED_CT) and
 * 3 (BUILTIN_CT) mean the module was calibrated together with the sensor it
 * ships with; a class or model write would replace that pairing with a
 * generic preset (the firmware clears the models even on an unchanged class
 * value, and re-applies a generic preset even on an unchanged model). */
typedef enum {
    RBAMPI_WRITE_PROCEED = 0,
    RBAMPI_WRITE_NOOP    = 1,  /* same value already on the module: no bus write */
    RBAMPI_WRITE_PAIRED  = 2,  /* factory-paired module: refuse (ESP_ERR_INVALID_STATE) */
} rbampi_write_policy_t;

bool rbampi_sensor_class_paired(uint8_t cls);
rbampi_write_policy_t rbampi_sensor_class_write_policy(uint8_t current_cls, uint8_t new_cls);
rbampi_write_policy_t rbampi_ct_model_write_policy(uint8_t current_cls, uint8_t current_model,
                                                   uint8_t new_model, bool force);

/* Whole-request decision for rbamp_configure_channels, taken BEFORE any bus
 * write: PAIRED if the module is class 2/3 and the request would change the
 * class or any requested (non-zero) model; NOOP if nothing would change;
 * PROCEED otherwise. current_models[ch] = model the channel holds now
 * (0xFF = unreadable). */
rbampi_write_policy_t rbampi_configure_policy(uint8_t current_cls, uint8_t new_cls,
                                              const uint8_t *current_models,
                                              const uint8_t *models, uint8_t n);

/* BASIC digest block (0x70): [STATUS][SEQ] then, only the enabled fields, in
 * this order: I u16 LE x n_ch (0.01 A), U u16 LE (0.1 V), P i16 LE x n_ch (W),
 * PF u8 x n_ch (|PF| x 100). n_ch = min(channels, 3). */
#define RBAMPI_DIGEST_MAX_CH    3u
#define RBAMPI_DIGEST_MASK_ALL  0x0Fu
#define RBAMPI_STATUS_EVENTS_PENDING (1u << 2)   /* STATUS bit2 = EVENT_FLAGS != 0 (v1.3+) */

typedef struct { uint8_t len, off_i, off_u, off_p, off_pf; } rbampi_digest_layout_t;

/* false when mask is 0 / has unknown bits or n_ch is 0 / > 3. Offsets of
 * absent fields are 0. len <= RBAMP_V2_REG_DIGEST_SIZE always. */
bool rbampi_digest_layout(uint8_t mask, uint8_t n_ch, rbampi_digest_layout_t *L);
void rbampi_digest_decode(const uint8_t *b, uint8_t mask, uint8_t n_ch, rbamp_digest_t *out);

/* Re-arm decision from the digest's STATUS mirror and (when events are
 * pending) the EVENT_FLAGS byte: true once per RESET_OCCURRED episode. */
bool rbampi_digest_needs_rearm(bool *reset_seen, bool events_pending, uint8_t event_flags);

rbampi_variant_result_t rbampi_basic_variant_apply(uint8_t hw, bool pinned,
                                                   uint8_t *channels, bool *has_voltage,
                                                   rbamp_variant_t *variant);

/* SCT-013 rating switch (libs/spec/SCT_RATING_SWITCH.md): on BASIC firmware
 * up to 1.4.19 a class-1 module keeps the factory 030 calibration after a
 * model switch; the library scales by preset(X) x c(X) / preset(030). */
/* CAPABILITY bit 12 (0x57/0x58): the module corrects the gain itself on a
 * model switch (fw 1.4.20+). Reserved by root in the spec; not in the
 * generated header yet — switch to RBAMP_V2_CAP_SCT_RATING_SELF when codegen
 * carries it. Feature detection by CAPABILITY bits only, never by VERSION. */
#define RBAMPI_CAP_SCT_RATING_SELF   (1u << 12)
#define RBAMPI_SCT_CAL_MODEL         3u     /* every batch is calibrated on SCT-013-030 */
#define RBAMPI_SCT_GAIN_TOL          0.05f  /* |G_applied / preset(030) - 1| beyond this = not a 030-calibrated module */
/* Channel-window (0x32) fields for applied gain / noise floor on ch3+; not in
 * the generated header yet (SCT_RATING_SWITCH.md names them 5 and 4). */
#define RBAMPI_CHFIELD_GAIN          5u
#define RBAMPI_CHFIELD_NF            4u

float rbampi_sct_preset_gain(uint8_t code);   /* fw preset G for an SCT-013 model code; 0 = no preset */
float rbampi_sct_rating_corr(uint8_t code);   /* bench rating correction c(X); 1.0 except the -100 */

/* The per-channel decision. capability = the module's CAPABILITY word
 * (CAP_SCT_RATING_SELF set = the module corrects itself, nothing to do);
 * g_applied = the gain the module applies now;
 * after_reset = this is the re-read after the one CMD_RESET. *need_reset is
 * set (with status PENDING) when g_applied is bit-equal to the raw preset of
 * a model other than 030 and no reset was issued yet. *scale = 1 unless
 * CORRECTED. */
rbamp_sct_status_t rbampi_sct_decide(uint16_t capability, uint8_t cls, uint8_t model,
                                     float g_applied, bool after_reset,
                                     bool *need_reset, float *scale);

#ifdef __cplusplus
}
#endif
