/* ============================================================================
 * AUTO-GENERATED from libs/spec/sensor_models.yaml — DO NOT EDIT
 *
 * Sensor-model registry: what a CT_MODEL code MEANS (name + descriptor).
 * Which codes a given module ACCEPTS is runtime truth — write the code and
 * check for ERR_PARAM; an unknown code leaves the channel's model unchanged.
 * Calibration constants (G/NF) are firmware presets and are not exposed here.
 *
 * Regenerate:
 *     python tools/lib_codegen/codegen_models.py
 * ============================================================================ */


#ifndef RBAMP_SENSOR_MODELS_H
#define RBAMP_SENSOR_MODELS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBAMP_SENSOR_MODELS_CRC32 0x26E5AB57u
#define RBAMP_SENSOR_MODELS_VERSION 1

/* ---- Sensor classes (REG_SENSOR_CLASS 0x25) ---- */
#define RBAMP_SENSOR_CLASS_UNSET           0u
#define RBAMP_SENSOR_CLASS_SCT_013         1u
#define RBAMP_SENSOR_CLASS_WIRED_CT        2u
#define RBAMP_SENSOR_CLASS_BUILTIN_CT      3u

/* ---- Model codes, unique per (class, code) ---- */
/* class 1: SCT_013 */
#define RBAMP_CT_SCT013_005                1u
#define RBAMP_CT_SCT013_010                2u
#define RBAMP_CT_SCT013_030                3u
#define RBAMP_CT_SCT013_050                4u
#define RBAMP_CT_SCT013_100                5u
#define RBAMP_CT_SCT013_020                6u
#define RBAMP_CT_SCT013_060                7u
/* class 2: WIRED_CT */
#define RBAMP_CT_TOROID_1000               1u
#define RBAMP_CT_TOROID_2000               2u
#define RBAMP_CT_TOROID_3000               3u
#define RBAMP_CT_CT2000_92R                4u
#define RBAMP_CT_CLAMP2000_92R             5u
#define RBAMP_CT_CLAMP2000_1A              6u
#define RBAMP_CT_CLAMP2000_5A              7u
#define RBAMP_CT_CLAMP2000_10A             8u
#define RBAMP_CT_CLAMP2000_20A             9u
#define RBAMP_CT_CLAMP2000_30A             10u
#define RBAMP_CT_CLAMP2000_50A             11u
#define RBAMP_CT_CLAMP2000_80A             12u
#define RBAMP_CT_CLAMP2000_100A            13u

/* ---- Status: characterized = measured, safe to ship; reserved = code
 * allocated, module returns ERR_PARAM; legacy = shipped historically,
 * not re-verified. ---- */
#define RBAMP_MODEL_STATUS_CHARACTERIZED 0u
#define RBAMP_MODEL_STATUS_RESERVED      1u
#define RBAMP_MODEL_STATUS_LEGACY        2u

typedef struct {
    uint8_t     sensor_class;
    uint8_t     code;
    const char *name;        /* short name, RBAMP_CT_ prefix stripped */
    const char *descriptor;  /* human string for init-time logging */
    uint8_t     status;
} rbamp_sensor_model_info_t;

static const rbamp_sensor_model_info_t RBAMP_SENSOR_MODELS[] = {
    { 1,  1, "SCT013_005", "SCT-013-005, 5 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 1,  2, "SCT013_010", "SCT-013-010, 10 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 1,  3, "SCT013_030", "SCT-013-030, 30 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 1,  4, "SCT013_050", "SCT-013-050, 50 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 1,  5, "SCT013_100", "SCT-013-100, 100 A", RBAMP_MODEL_STATUS_RESERVED },
    { 1,  6, "SCT013_020", "SCT-013-020, 20 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 1,  7, "SCT013_060", "SCT-013-060, 60 A", RBAMP_MODEL_STATUS_RESERVED },
    { 2,  1, "TOROID_1000", "Toroid 1:1000, 196 R burden", RBAMP_MODEL_STATUS_LEGACY },
    { 2,  2, "TOROID_2000", "Toroid 1:2000, 370 R burden", RBAMP_MODEL_STATUS_LEGACY },
    { 2,  3, "TOROID_3000", "Toroid 1:3000, 572 R burden", RBAMP_MODEL_STATUS_LEGACY },
    { 2,  4, "CT2000_92R", "CT 2000:1, 92 R burden", RBAMP_MODEL_STATUS_LEGACY },
    { 2,  5, "CLAMP2000_92R", "Clamp 2000:1, 92 R burden", RBAMP_MODEL_STATUS_LEGACY },
    { 2,  6, "CLAMP2000_1A", "Clamp 2000:1, 1 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 2,  7, "CLAMP2000_5A", "Clamp 2000:1, 5 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 2,  8, "CLAMP2000_10A", "Clamp 2000:1, 10 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 2,  9, "CLAMP2000_20A", "Clamp 2000:1, 20 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 2, 10, "CLAMP2000_30A", "Clamp 2000:1, 30 A", RBAMP_MODEL_STATUS_RESERVED },
    { 2, 11, "CLAMP2000_50A", "Clamp 2000:1, 50 A", RBAMP_MODEL_STATUS_RESERVED },
    { 2, 12, "CLAMP2000_80A", "Clamp 2000:1, 80 A", RBAMP_MODEL_STATUS_CHARACTERIZED },
    { 2, 13, "CLAMP2000_100A", "Clamp 2000:1, 100 A", RBAMP_MODEL_STATUS_RESERVED },
};
#define RBAMP_SENSOR_MODEL_COUNT (sizeof(RBAMP_SENSOR_MODELS) / sizeof(RBAMP_SENSOR_MODELS[0]))

/* NULL if (class, code) is not in the registry. A registry hit does NOT
 * mean the connected module accepts the code — that is runtime truth. */
static inline const rbamp_sensor_model_info_t *
rbamp_sensor_model_lookup(uint8_t sensor_class, uint8_t code)
{
    for (unsigned i = 0; i < RBAMP_SENSOR_MODEL_COUNT; ++i) {
        if (RBAMP_SENSOR_MODELS[i].sensor_class == sensor_class &&
            RBAMP_SENSOR_MODELS[i].code == code) {
            return &RBAMP_SENSOR_MODELS[i];
        }
    }
    return (const rbamp_sensor_model_info_t *)0;
}

#ifdef __cplusplus
}
#endif

#endif /* RBAMP_SENSOR_MODELS_H */
