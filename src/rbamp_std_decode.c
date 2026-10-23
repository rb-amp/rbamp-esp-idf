/* Pure STD-line decode helpers + the energy delta rules. Compiled into the
 * component and, with stubs, by test/host — keep it free of IDF calls. */
#include "rbamp_std_decode.h"
#include <string.h>
#include <math.h>

void rbampi_energy_decode_fine(rbamp_std_energy_t *e, const uint8_t *b, bool reactive_fine)
{
#define F(off) rbampi_le_i64(b + ((off) - RBAMPI_ENERGY_FINE_OFF))
    e->act_import_uwh = F(RBAMP_STD_ENERGY_E_ACT_IMP_UWH);
    e->act_export_uwh = F(RBAMP_STD_ENERGY_E_ACT_EXP_UWH);
    e->act_net_uwh    = F(RBAMP_STD_ENERGY_E_ACT_NET_UWH);
    e->fine           = true;
    if (reactive_fine) {
        e->rea_import_uvarh = F(RBAMP_STD_ENERGY_E_REA_IMP_UVARH);
        e->rea_export_uvarh = F(RBAMP_STD_ENERGY_E_REA_EXP_UVARH);
        e->fine_reactive    = true;
    }
#undef F
}

void rbampi_energy_decode_group_fine(rbamp_std_energy_t *e, const uint8_t *g, uint8_t n)
{
    for (uint8_t c = 0; c < n && c < RBAMP_MAX_CHANNELS; ++c) {
        const uint8_t *p = g + (size_t)c * RBAMP_STD_GROUP_ENERGY_UWH_STRIDE;
        e->ch_import_uwh[c] = rbampi_le_i64(p + RBAMP_STD_GROUP_ENERGY_UWH_E_IMP);
        e->ch_export_uwh[c] = rbampi_le_i64(p + RBAMP_STD_GROUP_ENERGY_UWH_E_EXP);
    }
}

void rbampi_energy_fill_coarse(rbamp_std_energy_t *e, bool active_fine, bool reactive_fine, bool have_mwh)
{
    if (!active_fine) {
        e->fine = false;
        if (have_mwh) {
            e->act_import_uwh = e->act_import_mwh * 1000;
            e->act_export_uwh = e->act_export_mwh * 1000;
            e->act_net_uwh    = e->act_net_mwh    * 1000;
            for (uint8_t c = 0; c < e->channels && c < RBAMP_MAX_CHANNELS; ++c) {
                e->ch_import_uwh[c] = e->ch_import_mwh[c] * 1000;
                e->ch_export_uwh[c] = e->ch_export_mwh[c] * 1000;
            }
        } else {
            e->act_import_uwh = e->act_import_wh * 1000000;
            e->act_export_uwh = e->act_export_wh * 1000000;
            e->act_net_uwh    = e->act_net_wh    * 1000000;
        }
    }
    if (!reactive_fine) {
        e->fine_reactive    = false;
        e->rea_import_uvarh = e->rea_import_varh * 1000000;
        e->rea_export_uvarh = e->rea_export_varh * 1000000;
    }
}

bool rbampi_start_ua_valid(uint16_t ua)
{
    if (ua == 0) return true;
    return ua >= RBAMPI_START_UA_STEP && ua <= RBAMPI_START_UA_MAX && (ua % RBAMPI_START_UA_STEP) == 0;
}

bool rbampi_angle_decode(int16_t raw, bool fw_has_angle, float *deg)
{
    if (!fw_has_angle || raw == RBAMPI_ANGLE_NONE) { *deg = 0.0f; return false; }
    *deg = (float)raw * 0.01f;
    return true;
}

rbamp_std_phase_seq_t rbampi_phase_seq_decode(uint8_t raw)
{
    switch (raw) {
    case 1:  return RBAMP_STD_PHASE_SEQ_FORWARD;
    case 2:  return RBAMP_STD_PHASE_SEQ_REVERSE;
    case 3:  return RBAMP_STD_PHASE_SEQ_ABNORMAL;
    default: return RBAMP_STD_PHASE_SEQ_NA;
    }
}

esp_err_t rbamp_std_energy_delta(const rbamp_std_energy_t *prev, const rbamp_std_energy_t *now,
                                 rbamp_std_energy_t *delta, bool *module_restarted)
{
    if (!prev || !now) return ESP_ERR_INVALID_ARG;
    if (module_restarted) *module_restarted = now->uptime_s < prev->uptime_s;
    if (now->generation != prev->generation) return ESP_ERR_INVALID_STATE;
    /* Power-cut / restart resume rule: a counter may come back a little
     * below the last reading; that is lost energy, not a discontinuity —
     * clamp to 0. NET is always IMP − EXP of the clamped parts. */
#define _DELTA0(f) (((now->f) > (prev->f)) ? (now->f) - (prev->f) : 0)
    if (delta) {
        memset(delta, 0, sizeof(*delta));
        delta->act_import_wh    = _DELTA0(act_import_wh);
        delta->act_export_wh    = _DELTA0(act_export_wh);
        delta->act_net_wh       = delta->act_import_wh - delta->act_export_wh;
        delta->rea_import_varh  = _DELTA0(rea_import_varh);
        delta->rea_export_varh  = _DELTA0(rea_export_varh);
        delta->act_import_mwh   = _DELTA0(act_import_mwh);
        delta->act_export_mwh   = _DELTA0(act_export_mwh);
        delta->act_net_mwh      = delta->act_import_mwh - delta->act_export_mwh;
        delta->act_import_uwh   = _DELTA0(act_import_uwh);
        delta->act_export_uwh   = _DELTA0(act_export_uwh);
        delta->act_net_uwh      = delta->act_import_uwh - delta->act_export_uwh;
        delta->rea_import_uvarh = _DELTA0(rea_import_uvarh);
        delta->rea_export_uvarh = _DELTA0(rea_export_uvarh);
        delta->fine             = prev->fine && now->fine;
        delta->fine_reactive    = prev->fine_reactive && now->fine_reactive;
        delta->generation       = now->generation;
        delta->uptime_s         = now->uptime_s;
        delta->group_status     = now->group_status;
        /* Per-channel: both readings must carry the same channels. A store
         * status of "restarted at 0" is NOT a reason to drop them: the
         * restart itself shows as a generation change or an uptime drop, and
         * a counter below the previous reading is clamped like the totals. */
        if (now->channels && now->channels == prev->channels) {
            delta->channels = now->channels;
            for (uint8_t c = 0; c < now->channels && c < RBAMP_MAX_CHANNELS; ++c) {
                delta->ch_import_mwh[c] = _DELTA0(ch_import_mwh[c]);
                delta->ch_export_mwh[c] = _DELTA0(ch_export_mwh[c]);
                delta->ch_import_uwh[c] = _DELTA0(ch_import_uwh[c]);
                delta->ch_export_uwh[c] = _DELTA0(ch_export_uwh[c]);
            }
        }
    }
#undef _DELTA0
    return ESP_OK;
}

bool rbampi_basic_variant_shape(uint8_t hw, uint8_t *channels, bool *has_voltage)
{
    uint8_t ch; bool volt;
    switch (hw) {
        case RBAMP_VARIANT_UI1: ch = 1; volt = true;  break;
        case RBAMP_VARIANT_UI2: ch = 2; volt = true;  break;
        case RBAMP_VARIANT_UI3: ch = 3; volt = true;  break;
        case RBAMP_VARIANT_I1:  ch = 1; volt = false; break;
        case RBAMP_VARIANT_I2:  ch = 2; volt = false; break;
        case RBAMP_VARIANT_I3:  ch = 3; volt = false; break;
        case RBAMP_VARIANT_UI5: ch = 5; volt = true;  break;
        case RBAMP_VARIANT_UI7: ch = 7; volt = true;  break;
        default: return false;
    }
    if (channels)    *channels = ch;
    if (has_voltage) *has_voltage = volt;
    return true;
}

rbampi_variant_result_t rbampi_basic_variant_apply(uint8_t hw, bool pinned,
                                                   uint8_t *channels, bool *has_voltage,
                                                   rbamp_variant_t *variant)
{
    uint8_t ch = 0; bool volt = false;
    if (!rbampi_basic_variant_shape(hw, &ch, &volt)) return RBAMPI_VARIANT_UNKNOWN;
    *variant     = (rbamp_variant_t)hw;
    *has_voltage = volt;
    if (pinned) return (*channels == ch) ? RBAMPI_VARIANT_APPLIED : RBAMPI_VARIANT_KEPT_PIN;
    *channels = ch;
    return RBAMPI_VARIANT_APPLIED;
}

uint16_t rbampi_basic_commit_ms(uint32_t commit_cnt, uint32_t latch_ms)
{
    if (commit_cnt == 0 || latch_ms == 0) return RBAMPI_BASIC_COMMIT_MS_FAST;
    /* latch_ms only ever reads SHORT (never long), so the boundary is not the
     * midpoint 150 but 120 ms: a 100-ms image can never read above 100, a
     * 200-ms image reads 140 even when 30 % short. */
    const uint64_t tenths = (uint64_t)latch_ms * 10u / commit_cnt;   /* ms/commit x10 */
    return tenths < 1200u ? RBAMPI_BASIC_COMMIT_MS_FAST : RBAMPI_BASIC_COMMIT_MS_SLOW;
}

void rbampi_basic_window_decode(uint32_t commit_cnt, uint32_t latch_ms, uint32_t uptime_s,
                                rbampi_basic_window_t *w)
{
    w->commit_ms = rbampi_basic_commit_ms(commit_cnt, latch_ms);
    const uint64_t ms = (uint64_t)commit_cnt * w->commit_ms;
    w->window_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    w->uptime_s  = uptime_s;
    w->from_boot = uptime_s != 0
                   && (uint64_t)w->window_ms + RBAMPI_BASIC_FROM_BOOT_TOL_MS >= (uint64_t)uptime_s * 1000u;
    w->is_long   = w->window_ms > RBAMPI_BASIC_WINDOW_LONG_MS;
}

uint32_t rbampi_basic_period_dt(bool *resume_pending, uint32_t master_dt_ms,
                                const rbampi_basic_window_t *w, bool *module_restarted)
{
    if (*resume_pending) {
        *resume_pending   = false;
        *module_restarted = w->from_boot;
        return w->window_ms;
    }
    *module_restarted = false;
    return master_dt_ms;
}

bool rbampi_sensor_class_paired(uint8_t cls)
{
    return cls == RBAMP_SENSOR_CLASS_WIRED_CT || cls == RBAMP_SENSOR_CLASS_BUILTIN_CT;
}

rbampi_write_policy_t rbampi_sensor_class_write_policy(uint8_t current_cls, uint8_t new_cls)
{
    if (current_cls == new_cls)              return RBAMPI_WRITE_NOOP;
    if (rbampi_sensor_class_paired(current_cls)) return RBAMPI_WRITE_PAIRED;
    return RBAMPI_WRITE_PROCEED;
}

rbampi_write_policy_t rbampi_ct_model_write_policy(uint8_t current_cls, uint8_t current_model,
                                                   uint8_t new_model, bool force)
{
    if (force)                               return RBAMPI_WRITE_PROCEED;
    if (current_model == new_model)          return RBAMPI_WRITE_NOOP;
    if (rbampi_sensor_class_paired(current_cls)) return RBAMPI_WRITE_PAIRED;
    return RBAMPI_WRITE_PROCEED;
}

/* Sensor limits come from the generated registry (rbamp_sensor_models.h):
 * nominal_a = rated RMS current, fs_a = RMS at clip onset for a sine,
 * peak_a = instantaneous clip ceiling; 0 = unknown. */
bool rbamp_sensor_model_limits(uint8_t sensor_class, uint8_t code,
                               float *nominal_a, float *fs_a, float *peak_a)
{
    const rbamp_sensor_model_info_t *m = rbamp_sensor_model_lookup(sensor_class, code);
    if (nominal_a) *nominal_a = m ? m->nominal_a : 0.0f;
    if (fs_a)      *fs_a      = m ? m->fs_a      : 0.0f;
    if (peak_a)    *peak_a    = m ? m->peak_a    : 0.0f;
    return m != NULL;
}

float rbamp_sensor_model_rated_a(uint8_t sensor_class, uint8_t code)
{
    float nominal = 0.0f;
    (void)rbamp_sensor_model_limits(sensor_class, code, &nominal, NULL, NULL);
    return nominal;
}

rbampi_write_policy_t rbampi_configure_policy(uint8_t current_cls, uint8_t new_cls,
                                              const uint8_t *current_models,
                                              const uint8_t *models, uint8_t n)
{
    bool change = false;
    switch (rbampi_sensor_class_write_policy(current_cls, new_cls)) {
        case RBAMPI_WRITE_PAIRED:  return RBAMPI_WRITE_PAIRED;
        case RBAMPI_WRITE_PROCEED: change = true; break;
        default: break;
    }
    for (uint8_t ch = 0; ch < n; ++ch) {
        if (models[ch] == 0) continue;
        switch (rbampi_ct_model_write_policy(current_cls, current_models[ch], models[ch], false)) {
            case RBAMPI_WRITE_PAIRED:  return RBAMPI_WRITE_PAIRED;
            case RBAMPI_WRITE_PROCEED: change = true; break;
            default: break;
        }
    }
    return change ? RBAMPI_WRITE_PROCEED : RBAMPI_WRITE_NOOP;
}

bool rbampi_digest_layout(uint8_t mask, uint8_t n_ch, rbampi_digest_layout_t *L)
{
    L->len = 0; L->off_i = L->off_u = L->off_p = L->off_pf = 0;
    if (mask == 0 || (mask & ~RBAMPI_DIGEST_MASK_ALL) || n_ch == 0 || n_ch > RBAMPI_DIGEST_MAX_CH) return false;
    uint8_t o = 2;
    if (mask & RBAMP_V2_DIGEST_I_RMS)  { L->off_i  = o; o += 2u * n_ch; }
    if (mask & RBAMP_V2_DIGEST_U_RMS)  { L->off_u  = o; o += 2u; }
    if (mask & RBAMP_V2_DIGEST_P_REAL) { L->off_p  = o; o += 2u * n_ch; }
    if (mask & RBAMP_V2_DIGEST_PF)     { L->off_pf = o; o += n_ch; }
    L->len = o;
    return true;
}

void rbampi_digest_decode(const uint8_t *b, uint8_t mask, uint8_t n_ch, rbamp_digest_t *out)
{
    rbampi_digest_layout_t L;
    memset(out, 0, sizeof *out);
    if (!rbampi_digest_layout(mask, n_ch, &L)) return;
    out->status = b[0];
    out->seq    = b[1];
    out->n_ch   = n_ch;
    out->fields = mask;
    for (uint8_t c = 0; c < n_ch; ++c) {
        if (L.off_i)  out->i_a[c] = (float)rbampi_le_u16(b + L.off_i + 2u * c) * 0.01f;
        if (L.off_p)  out->p_w[c] = rbampi_le_i16(b + L.off_p + 2u * c);
        if (L.off_pf) out->pf[c]  = (float)b[L.off_pf + c] * 0.01f;
    }
    if (L.off_u) out->u_v = (float)rbampi_le_u16(b + L.off_u) * 0.1f;
}

bool rbampi_digest_needs_rearm(bool *reset_seen, bool events_pending, uint8_t event_flags)
{
    if (!events_pending || !(event_flags & RBAMP_V2_EVENT_RESET_OCCURRED)) {
        *reset_seen = false;
        return false;
    }
    if (*reset_seen) return false;      /* same RESET episode, already re-armed */
    *reset_seen = true;
    return true;
}

const char *rbamp_basic_fw_name(uint8_t version)
{
    switch (version) {
        case 0x0D: return "1.4.21";
        case 0x0C: return "1.4.20";
        case 0x0B: return "1.4.19";
        case 0x0A: return "1.4.18";
        case 0x07: return "1.4.15-1.4.17";
        case 0x05: return "1.4.4-1.4.14";
        case 0x04: return "1.2-1.4.3";
        default:   return version <= 0x03 ? "<1.2" : NULL;
    }
}

/* -------------------------------------------------------------------------
 * SCT-013 rating switch
 * ------------------------------------------------------------------------- */

float rbampi_sct_preset_gain(uint8_t code)
{
    switch (code) {                 /* fw ct_presets.c, op-amp revision */
        case 1:  return 0.1058f;    /* -005 */
        case 2:  return 0.2116f;    /* -010 */
        case 6:  return 0.4233f;    /* -020 */
        case 3:  return 0.6349f;    /* -030, the calibration model */
        case 4:  return 1.0582f;    /* -050 */
        case 7:  return 1.2698f;    /* -060 */
        case 5:  return 2.1622f;    /* -100 */
        default: return 0.0f;
    }
}

float rbampi_sct_rating_corr(uint8_t code)
{
    return (code == 5) ? 0.962f : 1.0f;   /* -100 preset reads +4 % on the bench and the hub */
}

rbamp_sct_status_t rbampi_sct_decide(uint16_t capability, uint8_t cls, uint8_t model,
                                     float g_applied, bool after_reset,
                                     bool *need_reset, float *scale)
{
    *need_reset = false;
    *scale = 1.0f;
    if (capability & RBAMPI_CAP_SCT_RATING_SELF) return RBAMP_SCT_NONE;   /* the module corrects itself */
    if (cls != 1u) return RBAMP_SCT_NONE;                                   /* class 2/3: factory pair, leave alone */
    if (model == 0u) return RBAMP_SCT_NONE;                                 /* no sensor bound yet */
    const float preset = rbampi_sct_preset_gain(model);
    if (preset == 0.0f) return RBAMP_SCT_UNKNOWN_MODEL;
    if (model == RBAMPI_SCT_CAL_MODEL) return RBAMP_SCT_NONE;
    if (!isfinite(g_applied) || g_applied <= 0.0f) return RBAMP_SCT_GAIN_UNEXPECTED;
    uint32_t ga, gp;
    memcpy(&ga, &g_applied, 4);
    memcpy(&gp, &preset, 4);
    if (ga == gp) {                 /* raw preset applied: switched in this boot, trim k unknown */
        if (!after_reset) { *need_reset = true; return RBAMP_SCT_PENDING; }
        return RBAMP_SCT_UNCALIBRATED;
    }
    const float cal = rbampi_sct_preset_gain(RBAMPI_SCT_CAL_MODEL);
    if (fabsf(g_applied / cal - 1.0f) > RBAMPI_SCT_GAIN_TOL) return RBAMP_SCT_GAIN_UNEXPECTED;
    /* s = preset(X) x c(X) x k / G_a with k = G_a / preset(030): G_a cancels. */
    *scale = preset * rbampi_sct_rating_corr(model) / cal;
    return RBAMP_SCT_CORRECTED;
}
