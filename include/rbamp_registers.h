/* ============================================================================
 * AUTO-GENERATED from libs/spec/registers.yaml — DO NOT EDIT
 *
 * Regenerate with:  python tools/lib_codegen/codegen.py
 *
 * Any divergence between this file and libs/spec/registers.yaml is a bug.
 * tools/lib_codegen/parity_check.py enforces consistency across all 5 libs.
 * ============================================================================ */


#ifndef RBAMP_REGISTERS_H
#define RBAMP_REGISTERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBAMP_REG_SCHEMA_CRC32    0x8D38A1C2U
#define RBAMP_PROTOCOL_VERSION    0x014CU   /* 1.3 */

/* ---- Register addresses ---- */
#define RBAMP_REG_STATUS                     0x00u
#define RBAMP_REG_COMMAND                    0x01u
#define RBAMP_REG_ERROR                      0x02u
#define RBAMP_REG_VERSION                    0x03u
#define RBAMP_REG_MODE                       0x04u
#define RBAMP_REG_CT_MODEL                   0x05u
#define RBAMP_REG_V03_PHASE_SAMPLES          0x06u
#define RBAMP_REG_V03_PERIOD_VALID           0x07u
#define RBAMP_REG_LUT_VALID_MASK             0x08u
#define RBAMP_REG_LUT_QUERY_SLOT             0x09u
#define RBAMP_REG_LUT_VIEW_TIER              0x0Au
#define RBAMP_REG_LUT_VIEW_POINTS_LOG2       0x0Bu
#define RBAMP_REG_LUT_VIEW_INL_MAX_L         0x0Cu
#define RBAMP_REG_LUT_VIEW_INL_MAX_H         0x0Du
#define RBAMP_REG_LUT_VIEW_DNL_MAX_L         0x0Eu
#define RBAMP_REG_LUT_VIEW_DNL_MAX_H         0x0Fu
#define RBAMP_REG_DIM0_LEVEL                 0x10u
#define RBAMP_REG_DIM0_CURVE                 0x11u
#define RBAMP_REG_DIM0_FADE_TIME             0x18u
#define RBAMP_REG_AC_FREQ                    0x20u
#define RBAMP_REG_AC_PERIOD_L                0x21u
#define RBAMP_REG_AC_PERIOD_H                0x22u
#define RBAMP_REG_CALIBRATION                0x23u
#define RBAMP_REG_TOPOLOGY                   0x24u
#define RBAMP_REG_SENSOR_CLASS               0x25u
#define RBAMP_REG_V03_PHASE_FRACT            0x26u
#define RBAMP_REG_FLEET_CONFIG               0x27u
#define RBAMP_REG_GROUP_ID                   0x28u
#define RBAMP_REG_I2C_ADDRESS                0x30u
#define RBAMP_REG_ADDR_COMMIT_MAGIC          0x31u
#define RBAMP_REG_TEMP_T_WARN                0x36u
#define RBAMP_REG_TEMP_T_DERATE              0x37u
#define RBAMP_REG_TEMP_T_CRIT                0x38u
#define RBAMP_REG_TEMP_T_SHUTDOWN            0x39u
#define RBAMP_REG_TEMP_HYST                  0x3Au
#define RBAMP_REG_TEMP_CONFIG                0x3Bu
#define RBAMP_REG_TEMP_CURRENT               0x40u
#define RBAMP_REG_TEMP_STATE                 0x41u
#define RBAMP_REG_TEMP_MAX_LEVEL             0x42u
#define RBAMP_REG_TEMP_FLAGS                 0x43u
#define RBAMP_REG_TEMP_PEAK                  0x44u
#define RBAMP_REG_TEMP_RATE                  0x45u
#define RBAMP_REG_FAN_SPEED                  0x50u
#define RBAMP_REG_FAN_TARGET                 0x51u
#define RBAMP_REG_FAN_MODE                   0x52u
#define RBAMP_REG_FAN_STATUS                 0x53u
#define RBAMP_REG_CS_CONFIG                  0x54u
#define RBAMP_REG_HW_VARIANT                 0x55u
#define RBAMP_REG_CS_INTERVAL_H              0x56u
#define RBAMP_REG_CS0_SENSOR_TYPE            0x57u
#define RBAMP_REG_ACC_SEL                    0x58u
#define RBAMP_REG_COMMIT                     0x59u
#define RBAMP_REG_CS0_MODE                   0x5Au
#define RBAMP_REG_CS0_NOISE_FLOOR            0x5Bu
#define RBAMP_REG_CS_PERIOD_BUFS_2           0x5Cu
#define RBAMP_REG_CS_PERIOD_BUFS_3           0x5Du
#define RBAMP_REG_CS_PERIOD_BUFS_L           0x5Eu
#define RBAMP_REG_CS_PERIOD_BUFS_H           0x5Fu
#define RBAMP_REG_CS0_STATUS                 0x60u
#define RBAMP_REG_CS0_RMS_L                  0x61u
#define RBAMP_REG_CS0_RMS_H                  0x62u
#define RBAMP_REG_CS0_PEAK_L                 0x63u
#define RBAMP_REG_CS0_PEAK_H                 0x64u
#define RBAMP_REG_CS0_DIR                    0x65u
#define RBAMP_REG_CS0_PERIOD_IDX             0x66u
#define RBAMP_REG_CS0_DUR_0                  0x67u
#define RBAMP_REG_CS0_DUR_1                  0x68u
#define RBAMP_REG_CS0_DUR_2                  0x69u
#define RBAMP_REG_CS0_DUR_3                  0x6Au
#define RBAMP_REG_CS0_SMPL_0                 0x6Bu
#define RBAMP_REG_CS0_SMPL_1                 0x6Cu
#define RBAMP_REG_CS0_SMPL_2                 0x6Du
#define RBAMP_REG_CS0_SMPL_3                 0x6Eu
#define RBAMP_REG_CS0_MIN_L                  0x6Fu
#define RBAMP_REG_CS0_MIN_H                  0x70u
#define RBAMP_REG_CS0_MAX_L                  0x71u
#define RBAMP_REG_CS0_MAX_H                  0x72u
#define RBAMP_REG_CS0_DC_L                   0x73u
#define RBAMP_REG_CS0_DC_H                   0x74u
#define RBAMP_REG_CS0_CREST_L                0x75u
#define RBAMP_REG_CS0_CREST_H                0x76u
#define RBAMP_REG_CS0_RESERVED               0x77u
#define RBAMP_REG_VS_STATUS                  0x78u
#define RBAMP_REG_VS_RMS_L                   0x79u
#define RBAMP_REG_VS_RMS_H                   0x7Au
#define RBAMP_REG_VS_PEAK_L                  0x7Bu
#define RBAMP_REG_VS_PEAK_H                  0x7Cu
#define RBAMP_REG_VS_RATIO                   0x7Du
#define RBAMP_REG_CHARGE_Q_0                 0x7Eu
#define RBAMP_REG_CHARGE_Q_1                 0x7Fu
#define RBAMP_REG_CHARGE_Q_2                 0x80u
#define RBAMP_REG_CHARGE_Q_3                 0x81u
#define RBAMP_REG_CHARGE_N_0                 0x82u
#define RBAMP_REG_CHARGE_N_1                 0x83u
#define RBAMP_REG_CHARGE_N_2                 0x84u
#define RBAMP_REG_CHARGE_N_3                 0x85u
#define RBAMP_REG_V03_U_RMS                  0x86u
#define RBAMP_REG_V03_U_PEAK                 0x8Au
#define RBAMP_REG_V03_I0_RMS                 0x8Eu
#define RBAMP_REG_V03_I1_RMS                 0x92u
#define RBAMP_REG_V03_I2_RMS                 0x96u
#define RBAMP_REG_V03_I0_PEAK                0x9Au
#define RBAMP_REG_V03_I1_PEAK                0x9Eu
#define RBAMP_REG_V03_I2_PEAK                0xA2u
#define RBAMP_REG_V03_P0_REAL                0xA6u
#define RBAMP_REG_V03_P1_REAL                0xAAu
#define RBAMP_REG_V03_P2_REAL                0xAEu
#define RBAMP_REG_V03_PF0                    0xB2u
#define RBAMP_REG_V03_PF1                    0xB6u
#define RBAMP_REG_V03_PF2                    0xBAu
#define RBAMP_REG_V03_PERIOD_COMMIT_CNT      0xBEu
#define RBAMP_REG_V03_PERIOD_AVG_P_F1        0xC2u
#define RBAMP_REG_V03_PERIOD_AVG_P_F2        0xC6u
#define RBAMP_REG_V03_PERIOD_MS_B0           0xCAu
#define RBAMP_REG_V03_STATUS                 0xCEu
#define RBAMP_REG_V03_RESERVED               0xCFu
#define RBAMP_REG_CAL_CH_SEL                 0xD0u
#define RBAMP_REG_CAL_SAMPLES_N              0xD1u
#define RBAMP_REG_CAL_MEAN_L                 0xD2u
#define RBAMP_REG_CAL_MEAN_H                 0xD3u
#define RBAMP_REG_CAL_STDDEV_L               0xD4u
#define RBAMP_REG_CAL_STDDEV_H               0xD5u
#define RBAMP_REG_CAL_MIN_L                  0xD6u
#define RBAMP_REG_CAL_MIN_H                  0xD7u
#define RBAMP_REG_CAL_MAX_L                  0xD8u
#define RBAMP_REG_CAL_MAX_H                  0xD9u
#define RBAMP_REG_CAL_STATE                  0xDAu
#define RBAMP_REG_CAL_ERROR                  0xDBu
#define RBAMP_REG_V03_PERIOD_AVG_P_F0        0xDCu
#define RBAMP_REG_V03_PERIOD_MAX_P_F0        0xE0u
#define RBAMP_REG_V03_PERIOD_LATCH_MS        0xECu
#define RBAMP_REG_V03_U_NOISE_FLOOR_L        0xE4u
#define RBAMP_REG_V03_U_NOISE_FLOOR_H        0xE5u
#define RBAMP_REG_V03_I0_NOISE_FLOOR_L       0xE6u
#define RBAMP_REG_V03_I0_NOISE_FLOOR_H       0xE7u
#define RBAMP_REG_V03_I1_NOISE_FLOOR_L       0xE8u
#define RBAMP_REG_V03_I1_NOISE_FLOOR_H       0xE9u
#define RBAMP_REG_V03_I2_NOISE_FLOOR_L       0xEAu
#define RBAMP_REG_V03_I2_NOISE_FLOOR_H       0xEBu
#define RBAMP_REG_V03_U_GAIN                 0xF0u
#define RBAMP_REG_V03_I0_GAIN                0xF4u
#define RBAMP_REG_V03_I1_GAIN                0xF8u
#define RBAMP_REG_V03_I2_GAIN                0xFCu

/* ---- Command opcodes ---- */
#define RBAMP_CMD_NOP                        0x00u
#define RBAMP_CMD_RESET                      0x01u
#define RBAMP_CMD_RECALIBRATE                0x02u
#define RBAMP_CMD_SWITCH_UART                0x03u
#define RBAMP_CMD_CHARGE_RESET               0x05u
#define RBAMP_CMD_CAL_BEGIN                  0x20u
#define RBAMP_CMD_CAL_SAMPLE                 0x21u
#define RBAMP_CMD_CAL_LUT_WRITE              0x22u
#define RBAMP_CMD_CAL_LUT_COMMIT             0x23u
#define RBAMP_CMD_CAL_LUT_ABORT              0x24u
#define RBAMP_CMD_CAL_END                    0x25u
#define RBAMP_CMD_SAVE_GAINS                 0x26u
#define RBAMP_CMD_LATCH_PERIOD               0x27u
#define RBAMP_CMD_SET_CT_MODEL_CH0           0x28u
#define RBAMP_CMD_SET_CT_MODEL_CH1           0x29u
#define RBAMP_CMD_SET_CT_MODEL_CH2           0x2Au
#define RBAMP_CMD_COMMIT_ADDR                0x30u
#define RBAMP_CMD_SAVE_USER_CONFIG           0x32u
#define RBAMP_CMD_FACTORY_RESET              0xAAu

/* ---- Command settle times (ms) ---- */
#define RBAMP_SETTLE_MS_NOP                    0u
#define RBAMP_SETTLE_MS_RESET                  300u
#define RBAMP_SETTLE_MS_RECALIBRATE            200u
#define RBAMP_SETTLE_MS_SWITCH_UART            50u
#define RBAMP_SETTLE_MS_CHARGE_RESET           5u
#define RBAMP_SETTLE_MS_CAL_BEGIN              10u
#define RBAMP_SETTLE_MS_CAL_SAMPLE             50u
#define RBAMP_SETTLE_MS_CAL_LUT_WRITE          5u
#define RBAMP_SETTLE_MS_CAL_LUT_COMMIT         700u
#define RBAMP_SETTLE_MS_CAL_LUT_ABORT          5u
#define RBAMP_SETTLE_MS_CAL_END                50u
#define RBAMP_SETTLE_MS_SAVE_GAINS             700u
#define RBAMP_SETTLE_MS_LATCH_PERIOD           50u
#define RBAMP_SETTLE_MS_SET_CT_MODEL_CH0       5u
#define RBAMP_SETTLE_MS_SET_CT_MODEL_CH1       5u
#define RBAMP_SETTLE_MS_SET_CT_MODEL_CH2       5u
#define RBAMP_SETTLE_MS_COMMIT_ADDR            700u
#define RBAMP_SETTLE_MS_SAVE_USER_CONFIG       700u
#define RBAMP_SETTLE_MS_FACTORY_RESET          1500u

/* ---- Device error codes ---- */
#define RBAMP_DEV_ERR_OK                     0x00u
#define RBAMP_DEV_ERR_LUT_BAD                0xFAu
#define RBAMP_DEV_ERR_FLASH_PARAMS_BAD       0xFBu
#define RBAMP_DEV_ERR_NOT_READY              0xFCu
#define RBAMP_DEV_ERR_SENSOR_OVERFLOW        0xFDu
#define RBAMP_DEV_ERR_PARAM                  0xFEu
#define RBAMP_DEV_ERR_UNHANDLED              0xFFu

/* ---- Library error codes ---- */
#define RBAMP_OK                           (0)
#define RBAMP_ERR_IO                       (-1)
#define RBAMP_ERR_NACK                     (-2)
#define RBAMP_ERR_TIMEOUT                  (-3)
#define RBAMP_ERR_NOT_READY                (-4)
#define RBAMP_ERR_STALE                    (-5)
#define RBAMP_ERR_PARAM                    (-6)
#define RBAMP_ERR_MODE                     (-7)
#define RBAMP_ERR_CHECKSUM                 (-8)
#define RBAMP_ERR_VERSION                  (-9)
#define RBAMP_ERR_NOT_IMPLEMENTED          (-10)
#define RBAMP_ERR_NON_PHYSICAL             (-11)

#ifdef __cplusplus
}
#endif

#endif /* RBAMP_REGISTERS_H */
