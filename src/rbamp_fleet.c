/**
 * @file    rbamp_fleet.c
 * @brief   Implementation of the rbAmp multi-module / fleet manager (v1.3).
 * @see     rbamp_fleet.h
 */
#include "sdkconfig.h"

#if defined(CONFIG_RBAMP_LOG_LEVEL)
#  define LOG_LOCAL_LEVEL CONFIG_RBAMP_LOG_LEVEL
#endif

#include "rbamp_fleet.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rbamp_fleet";

#ifndef CONFIG_RBAMP_I2C_TIMEOUT_MS
#  define CONFIG_RBAMP_I2C_TIMEOUT_MS 100
#endif

struct rbamp_fleet_obj_t {
    i2c_master_bus_handle_t bus;
    rbamp_handle_t          dev[RBAMP_FLEET_MAX_MODULES];
    bool                    owned[RBAMP_FLEET_MAX_MODULES]; /**< free on destroy? */
    size_t                  count;

    /* Addresses scan flagged as a suspected duplicate-address conflict and
     * EXCLUDED from the fleet — never polled, surfaced for the operator. */
    uint8_t                 excluded[RBAMP_FLEET_MAX_MODULES];
    size_t                  n_excluded;
};

/** @internal Probe an address across the post-reset boot window.
 *
 * A module that just committed an address change has rebooted (~300 ms boot +
 * first-period warmup) and NACKs the verify probe transiently. A torn/early
 * probe must NOT fail an already-committed provision/assign (the address is
 * persisted in flash at that point — re-running would then refuse "target
 * busy"). Retry across ~600 ms before declaring the module absent. Used ONLY
 * for the post-commit verify, not for the pre-commit "is anyone here" checks
 * (those want a fast single probe). standfw fleet-harden #1. */
static bool _probe_retry(i2c_master_bus_handle_t bus, uint8_t addr)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (i2c_master_probe(bus, addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return false;
}

/** @internal Address-collision probe — two complementary signals.
 *
 * Physics: two slaves at one address clock out on the SHARED SCL, so SDA is a
 * clean, STABLE bitwise-AND (open-drain, 0 wins). So:
 *
 * SIGNAL 1 — static-identity cross-consistency (catches DIFFERENT-variant
 * idle, standfw exclude-bench). The wired-AND of two different-variant modules
 * produces an identity that is internally INCONSISTENT: e.g. I2 (HW=5) + UI1
 * (HW=1) AND to HW_VARIANT=1 ("UI1"), but a real UI1 advertises the voltage /
 * ZC-phase capability whereas the ANDed CAPABILITY bit = AND(UI1=1, I2=0) = 0.
 * A coherent module agrees: U-variants (UI1/2/3) carry the voltage cap, I-
 * variants (I1/2/3) do not. A mismatch between HW_VARIANT's implied voltage
 * profile and the CAPABILITY register is a deterministic conflict signal.
 *   Voltage indicator = CAPABILITY bit 8 (@c RBAMP_V2_CAP_ZC_PHASE_OFFSET) —
 *   the STABLE invariant; the other capability bits vary by firmware build
 *   (current v1.3 source: base 0x069E / U-variant 0x079E; standfw bench
 *   modules read older 0x0618 / 0x0718). bit8 alone keys the U/I distinction
 *   and is source-validated (root 2026-06-16). The I2+UI1 wired-AND yields
 *   HW_VARIANT 0x01 ("UI1") with bit8 CLEAR (AND of SET·CLEAR) → inconsistent.
 *   Do NOT key this on SENSOR_CLASS (0x25) — that is user-config, not
 *   variant-intrinsic.
 *
 * SIGNAL 2 — live-value divergence (catches SAME-variant under DIFFERING load).
 * @c I0_RMS advances independently per module; two modules' AND of drifting
 * floats is frequently non-finite/out-of-range (sanity-rejected) or chaotic.
 *   Blind at idle: at zero load I0_RMS is exactly 0.0 (noise-floor subtracted)
 *   on every module → AND is a stable 0. Same-variant idle is therefore
 *   near-undetectable — the one-virgin-at-a-time discipline is the real
 *   defense; this is the best-effort safety net. Requires @c addr to ACK. */
static bool _suspect_collision(i2c_master_bus_handle_t bus, uint8_t addr)
{
    rbamp_handle_t s = NULL;
    /* rbamp_new defaults to THREE_PHASE, so channel 0 is always valid for the
     * live current read without running the full begin() primer. */
    if (rbamp_new(bus, addr, &s) != ESP_OK) return false;

    /* Signal 1 — identity cross-consistency. */
    rbamp_variant_t v = RBAMP_VARIANT_UNKNOWN;
    uint16_t cap = 0;
    if (rbamp_read_variant(s, &v) == ESP_OK && v != RBAMP_VARIANT_UNKNOWN
            && rbamp_read_capability(s, &cap) == ESP_OK) {
        bool variant_is_u = (v >= RBAMP_VARIANT_UI1 && v <= RBAMP_VARIANT_UI3);
        bool cap_has_voltage = (cap & RBAMP_V2_CAP_ZC_PHASE_OFFSET) != 0;
        if (variant_is_u != cap_has_voltage) {
            rbamp_del(s);
            return true;  /* HW_VARIANT vs CAPABILITY self-contradiction */
        }
    }

    /* Signal 2 — live-value divergence. */
    int rejects = 0, n_seen = 0;
    float seen[8];
    for (int i = 0; i < 8; ++i) {
        float val = 0.0f;
        esp_err_t e = rbamp_read_current(s, 0, &val);  /* live, sanity-checked */
        if (e == ESP_ERR_INVALID_RESPONSE) {
            rejects++;                                 /* ANDed garbage float */
        } else if (e == ESP_OK) {
            bool dup = false;
            for (int j = 0; j < n_seen; ++j) { if (seen[j] == val) { dup = true; break; } }
            if (!dup && n_seen < 8) seen[n_seen++] = val;
        }
        vTaskDelay(pdMS_TO_TICKS(15));                 /* ~120 ms total — under one 200 ms refresh; best-effort */
    }
    rbamp_del(s);
    return (rejects >= 2) || (n_seen >= 4);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_fleet_create(i2c_master_bus_handle_t bus, rbamp_fleet_t *out)
{
    if (!bus || !out) return ESP_ERR_INVALID_ARG;
    rbamp_fleet_t f = calloc(1, sizeof(*f));
    if (!f) return ESP_ERR_NO_MEM;
    f->bus = bus;
    f->count = 0;
    *out = f;
    return ESP_OK;
}

void rbamp_fleet_destroy(rbamp_fleet_t fleet)
{
    if (!fleet) return;
    for (size_t i = 0; i < fleet->count; ++i) {
        if (fleet->owned[i] && fleet->dev[i]) {
            rbamp_del(fleet->dev[i]);
        }
    }
    free(fleet);
}

/* -------------------------------------------------------------------------
 * Membership
 * ------------------------------------------------------------------------- */

size_t rbamp_fleet_count(rbamp_fleet_t fleet)
{
    return fleet ? fleet->count : 0;
}

rbamp_handle_t rbamp_fleet_get(rbamp_fleet_t fleet, size_t idx)
{
    if (!fleet || idx >= fleet->count) return NULL;
    return fleet->dev[idx];
}

rbamp_handle_t rbamp_fleet_find(rbamp_fleet_t fleet, uint8_t addr)
{
    if (!fleet) return NULL;
    for (size_t i = 0; i < fleet->count; ++i) {
        if (rbamp_address(fleet->dev[i]) == addr) return fleet->dev[i];
    }
    return NULL;
}

size_t rbamp_fleet_excluded_count(rbamp_fleet_t fleet)
{
    return fleet ? fleet->n_excluded : 0;
}

int rbamp_fleet_excluded_addr(rbamp_fleet_t fleet, size_t idx)
{
    if (!fleet || idx >= fleet->n_excluded) return -1;
    return (int)fleet->excluded[idx];
}

esp_err_t rbamp_fleet_poll_all(rbamp_fleet_t fleet,
                               rbamp_snapshot_t *snapshots,
                               rbamp_fleet_poll_t *status,
                               size_t cap, size_t *n_ok)
{
    if (!fleet || !snapshots || !status) return ESP_ERR_INVALID_ARG;
    if (cap < fleet->count) return ESP_ERR_INVALID_SIZE;
    size_t ok = 0;
    for (size_t i = 0; i < fleet->count; ++i) {
        rbamp_handle_t d = fleet->dev[i];
        status[i].addr = rbamp_address(d);
        status[i].channels = rbamp_channels(d);
        /* One module NACKing must not abort the whole poll — mark MISS, go on. */
        esp_err_t err = rbamp_read_all(d, &snapshots[i]);
        status[i].ok = (err == ESP_OK);
        if (status[i].ok) ok++;
        else memset(&snapshots[i], 0, sizeof(snapshots[i]));
    }
    if (n_ok) *n_ok = ok;
    return ESP_OK;
}

/** @internal Append a handle. Returns index or -1 if full. */
static int _fleet_append(rbamp_fleet_t fleet, rbamp_handle_t dev, bool owned)
{
    if (fleet->count >= RBAMP_FLEET_MAX_MODULES) return -1;
    int idx = (int)fleet->count;
    fleet->dev[idx] = dev;
    fleet->owned[idx] = owned;
    fleet->count++;
    return idx;
}

esp_err_t rbamp_fleet_add(rbamp_fleet_t fleet, rbamp_handle_t dev)
{
    if (!fleet || !dev) return ESP_ERR_INVALID_ARG;
    if (rbamp_fleet_find(fleet, rbamp_address(dev))) return ESP_ERR_INVALID_STATE;
    if (_fleet_append(fleet, dev, /*owned=*/false) < 0) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t rbamp_fleet_scan(rbamp_fleet_t fleet, bool match_product, size_t *added)
{
    if (!fleet) return ESP_ERR_INVALID_ARG;
    size_t n_added = 0;
    int canary = -1;   /* first confirmed-empty address — bus-health witness */
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (fleet->count >= RBAMP_FLEET_MAX_MODULES) break;
        if (rbamp_fleet_find(fleet, addr)) continue;
        if (i2c_master_probe(fleet->bus, addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) != ESP_OK) {
            if (canary < 0) canary = addr;  /* known-empty → tier-2 witness */
            continue;  /* no device at this address */
        }
        if (_suspect_collision(fleet->bus, addr)) {
            /* Tier 2 (operator addendum): is the conflict WEDGING the bus? A
             * known-empty address must still NACK. If the canary now ACKs, SDA
             * is stuck / the bus is corrupted → ABORT the whole init and hand
             * back an EMPTY fleet (safer than a fleet built on a broken bus). */
            if (canary >= 0 &&
                i2c_master_probe(fleet->bus, (uint8_t)canary,
                                 CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
                ESP_LOGE(TAG, "bus COMPROMISED by conflict at 0x%02X (empty 0x%02X "
                              "now ACKs) — fleet init aborted; fix wiring",
                         addr, canary);
                for (size_t i = 0; i < fleet->count; ++i) {
                    if (fleet->owned[i] && fleet->dev[i]) rbamp_del(fleet->dev[i]);
                }
                fleet->count = 0;
                fleet->n_excluded = 0;
                if (added) *added = 0;
                return ESP_ERR_INVALID_STATE;
            }
            /* Tier 1 — bus healthy: detect-and-EXCLUDE (operator design). The
             * conflict is not resolved here; the address is excluded so
             * poll_all/aggregation never touch its garbled wired-AND data.
             * Best-effort (catches differing-live / different-variant; idle
             * identical modules are near-undetectable — one-virgin discipline). */
            bool seen = false;
            for (size_t e = 0; e < fleet->n_excluded; ++e) {
                if (fleet->excluded[e] == addr) { seen = true; break; }
            }
            if (!seen && fleet->n_excluded < RBAMP_FLEET_MAX_MODULES) {
                fleet->excluded[fleet->n_excluded++] = addr;
            }
            ESP_LOGW(TAG, "0x%02X: suspected conflict — EXCLUDED from fleet; "
                          "provision one module at a time", addr);
            continue;
        }
        rbamp_handle_t dev = NULL;
        if (rbamp_new(fleet->bus, addr, &dev) != ESP_OK) continue;
        if (rbamp_begin(dev) != ESP_OK) {       /* not an rbAmp / no valid version */
            rbamp_del(dev);
            continue;
        }
        if (match_product) {
            uint8_t pid = 0;
            if (rbamp_read_product_id(dev, &pid) != ESP_OK || pid != 0x01) {
                ESP_LOGD(TAG, "0x%02X: product 0x%02X != rbAmp, skip", addr, pid);
                rbamp_del(dev);
                continue;
            }
        }
        if (_fleet_append(fleet, dev, /*owned=*/true) < 0) {
            rbamp_del(dev);
            break;
        }
        n_added++;
        ESP_LOGI(TAG, "added module at 0x%02X (%u ch, voltage=%s)",
                 addr, rbamp_channels(dev),
                 rbamp_has_voltage(dev) ? "yes" : "no");
    }
    if (added) *added = n_added;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Address assignment
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_fleet_check_conflict(rbamp_fleet_t fleet, uint8_t addr,
                                     bool *collision)
{
    if (!fleet || !collision) return ESP_ERR_INVALID_ARG;
    *collision = false;
    if (i2c_master_probe(fleet->bus, addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_OK;  /* nobody home — no conflict */
    }
    /* UID-based detection — VERSION is identical across same-variant modules
     * and would miss two same-firmware modules at one address (#C1). */
    *collision = _suspect_collision(fleet->bus, addr);
    if (*collision) {
        ESP_LOGW(TAG, "0x%02X: suspected address collision (live value diverges)", addr);
    }
    return ESP_OK;
}

esp_err_t rbamp_fleet_assign_address(rbamp_fleet_t fleet, rbamp_handle_t dev,
                                     uint8_t new_addr)
{
    if (!fleet || !dev) return ESP_ERR_INVALID_ARG;
    if (new_addr < 0x08 || new_addr > 0x77) return ESP_ERR_INVALID_ARG;
    if (new_addr == rbamp_address(dev)) return ESP_OK;  /* already there */

    /* Refuse a multiply-occupied SOURCE address (#C2): never two-phase an
     * address that ≥2 modules share — both would latch the candidate and the
     * conflict just moves. Detect via UID before issuing any write. */
    if (_suspect_collision(fleet->bus, rbamp_address(dev))) {
        ESP_LOGW(TAG, "assign refused: 0x%02X is multiply-occupied (collision)",
                 rbamp_address(dev));
        return ESP_ERR_INVALID_STATE;
    }

    /* Refuse if something already answers at the target address. */
    if (i2c_master_probe(fleet->bus, new_addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGW(TAG, "assign refused: 0x%02X already in use", new_addr);
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t old_addr = rbamp_address(dev);
    esp_err_t err = rbamp_prepare_address_change(dev, new_addr);
    if (err != ESP_OK) return err;
    err = rbamp_commit_address_change(dev);  /* handle re-binds to new_addr */
    if (err != ESP_OK) return err;

    /* Confirm the module answers at the new address — boot-window retry so a
     * transient post-reset NACK doesn't fail an already-committed assign. */
    if (!_probe_retry(fleet->bus, new_addr)) {
        ESP_LOGW(TAG, "assign: 0x%02X did not ACK after commit", new_addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* The old address must have vacated — if it still answers, the move was
     * partial or another module shared it (standfw fleet-harden #2). */
    if (i2c_master_probe(fleet->bus, old_addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGW(TAG, "assign: 0x%02X still answers after move — partial/conflict",
                 old_addr);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "assigned module 0x%02X → 0x%02X", old_addr, new_addr);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Standalone provisioning
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_provision(i2c_master_bus_handle_t bus, uint8_t desired_addr,
                          bool save_config, rbamp_handle_t *out_dev)
{
    if (!bus || !out_dev) return ESP_ERR_INVALID_ARG;
    if (desired_addr < 0x08 || desired_addr > 0x77 || desired_addr == 0x50) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_dev = NULL;

    /* 1. Exactly one virgin must answer at 0x50. */
    if (i2c_master_probe(bus, 0x50, CONFIG_RBAMP_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "provision: nothing at 0x50");
        return ESP_ERR_NOT_FOUND;
    }
    if (_suspect_collision(bus, 0x50)) {
        ESP_LOGE(TAG, "provision: suspected collision at 0x50 — "
                      "connect ONE virgin module at a time");
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. Target address must be free. */
    if (i2c_master_probe(bus, desired_addr, CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGW(TAG, "provision: 0x%02X already in use", desired_addr);
        return ESP_ERR_INVALID_STATE;
    }

    /* 3. Bind at 0x50, two-phase move, verify via probe at the new address. */
    rbamp_handle_t dev = NULL;
    esp_err_t err = rbamp_new(bus, 0x50, &dev);
    if (err != ESP_OK) return err;
    err = rbamp_begin(dev);
    if (err != ESP_OK) { rbamp_del(dev); return err; }

    err = rbamp_prepare_address_change(dev, desired_addr);
    if (err != ESP_OK) { rbamp_del(dev); return err; }
    err = rbamp_commit_address_change(dev);   /* handle re-binds to desired_addr */
    if (err != ESP_OK) { rbamp_del(dev); return err; }

    /* Boot-window retry: the module just rebooted, so a single probe NACKs
     * transiently and would falsely fail an already-committed provision. */
    if (!_probe_retry(bus, desired_addr)) {
        ESP_LOGE(TAG, "provision: 0x%02X did not ACK after commit", desired_addr);
        rbamp_del(dev);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Positive single-module confirmation (standfw fleet-harden #2): 0x50 must
     * now be EMPTY — the one module vacated it. If it still answers, the move
     * didn't take cleanly OR a second module was connected and stayed put. Fail
     * loud rather than leave a half-moved bus. (Two virgins that BOTH latched
     * and moved can't be caught here — idle identical modules are a clean
     * stable wired-AND, near-undetectable by reads; that is exactly why the
     * one-virgin-at-a-time precondition is a HARD requirement, not advisory.) */
    if (i2c_master_probe(bus, 0x50, CONFIG_RBAMP_I2C_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGE(TAG, "provision: 0x50 still answers after move — aborting "
                      "(more than one module on the bus? see one-virgin rule)");
        rbamp_del(dev);
        return ESP_ERR_INVALID_STATE;
    }

    /* 4. Fresh module → ERR_FLASH_PARAMS_BAD; repair + persist if requested. */
    if (save_config) {
        err = rbamp_save_user_config(dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "provision: SAVE_USER_CONFIG failed (%s) — module is at "
                          "0x%02X but unprovisioned", esp_err_to_name(err), desired_addr);
            /* not fatal: the address move succeeded */
        }
    }

    ESP_LOGI(TAG, "provisioned virgin → 0x%02X%s", desired_addr,
             save_config ? " (+saved)" : "");
    *out_dev = dev;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * GC fleet-sync
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_fleet_enable_gc_all(rbamp_fleet_t fleet, uint8_t group,
                                    size_t *ok_count)
{
    if (!fleet) return ESP_ERR_INVALID_ARG;
    size_t ok = 0;
    for (size_t i = 0; i < fleet->count; ++i) {
        rbamp_handle_t d = fleet->dev[i];
        if (group != 0) {
            if (rbamp_set_group_id(d, group) != ESP_OK) continue;
        }
        if (rbamp_enable_gc(d, true) == ESP_OK) ok++;
        else ESP_LOGW(TAG, "0x%02X: enable_gc failed", rbamp_address(d));
    }
    if (ok_count) *ok_count = ok;
    return ESP_OK;
}

esp_err_t rbamp_fleet_gclatch(rbamp_fleet_t fleet, uint8_t group, uint16_t tick,
                              uint32_t settle_ms)
{
    if (!fleet) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rbamp_broadcast_latch_group(fleet->bus, group, tick,
                                                CONFIG_RBAMP_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    return ESP_OK;
}

esp_err_t rbamp_fleet_check_sync(rbamp_fleet_t fleet, uint16_t expected_tick,
                                 rbamp_fleet_sync_t *status, size_t status_cap,
                                 size_t *n_missed)
{
    if (!fleet || !status) return ESP_ERR_INVALID_ARG;
    if (status_cap < fleet->count) return ESP_ERR_INVALID_SIZE;
    size_t missed = 0;
    for (size_t i = 0; i < fleet->count; ++i) {
        rbamp_handle_t d = fleet->dev[i];
        rbamp_fleet_sync_t *s = &status[i];
        s->addr = rbamp_address(d);
        s->gc_tick = 0xFFFF;
        s->in_sync = false;
        uint16_t t = 0xFFFF;
        s->reachable = (rbamp_read_gc_tick(d, &t) == ESP_OK);
        if (s->reachable) {
            s->gc_tick = t;
            s->in_sync = (t == expected_tick);
            if (!s->in_sync) missed++;
        } else {
            missed++;
        }
    }
    if (n_missed) *n_missed = missed;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Aggregation / monitoring
 * ------------------------------------------------------------------------- */

esp_err_t rbamp_fleet_total_power(rbamp_fleet_t fleet, float *out_w)
{
    if (!fleet || !out_w) return ESP_ERR_INVALID_ARG;
    float total = 0.0f;
    for (size_t i = 0; i < fleet->count; ++i) {
        rbamp_handle_t d = fleet->dev[i];
        uint8_t nch = rbamp_channels(d);
        for (uint8_t ch = 0; ch < nch; ++ch) {
            float p = 0.0f;
            if (rbamp_read_power(d, ch, &p) == ESP_OK) total += p;
        }
    }
    *out_w = total;
    return ESP_OK;
}

esp_err_t rbamp_fleet_total_energy_wh(rbamp_fleet_t fleet, double *out_wh)
{
    if (!fleet || !out_wh) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_RBAMP_DISABLE_ENERGY)
    *out_wh = 0.0;
    return ESP_ERR_NOT_SUPPORTED;  /* energy accumulator compiled out */
#else
    double total = 0.0;
    for (size_t i = 0; i < fleet->count; ++i) {
        rbamp_handle_t d = fleet->dev[i];
        uint8_t nch = rbamp_channels(d);
        for (uint8_t ch = 0; ch < nch; ++ch) {
            total += rbamp_energy_wh(d, ch);
        }
    }
    *out_wh = total;
    return ESP_OK;
#endif
}

esp_err_t rbamp_fleet_poll_errors(rbamp_fleet_t fleet, uint32_t *error_mask,
                                  size_t *n_errors)
{
    if (!fleet) return ESP_ERR_INVALID_ARG;
    uint32_t mask = 0;
    size_t n = 0;
    for (size_t i = 0; i < fleet->count; ++i) {
        bool has_err = false;
        if (rbamp_has_error(fleet->dev[i], &has_err) == ESP_OK && has_err) {
            if (i < 32) mask |= (1u << i);
            n++;
        }
    }
    if (error_mask) *error_mask = mask;
    if (n_errors) *n_errors = n;
    return ESP_OK;
}
