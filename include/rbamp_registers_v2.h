/* ============================================================================
 * AUTO-GENERATED from libs/spec/registers_v2.yaml — DO NOT EDIT
 *
 * Schema v2 (v1.3 wire contract). Regenerate:
 *     python tools/lib_codegen/codegen_v2.py
 *
 * Production-build registers only; the factory cal block (build: cal) is
 * intentionally absent — factory tooling reads the YAML directly.
 * parity_check via codegen_v2.py --check.
 * ============================================================================ */


#ifndef RBAMP_REGISTERS_V2_H
#define RBAMP_REGISTERS_V2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBAMP_V2_REG_SCHEMA_CRC32_V2           0x5FB3E9F3U
#define RBAMP_V2_PROTOCOL_VERSION_V2           0x0103U   /* 1.3 — (major<<8)|minor */

/* ---- Register addresses + sizes ---- */
#define RBAMP_V2_REG_STATUS                    0x00u   /* bit0=READY, bit1=ERROR, bit2=EVENTS_PENDING (v1.3: mirror of EVENT_FLAGS!=0) */
#define RBAMP_V2_REG_COMMAND                   0x01u   /* Write CMD_* opcode (commands.yaml) */
#define RBAMP_V2_REG_ERROR                     0x02u   /* 0x00=OK; 0xFA..0xFF error classes; ERR_CLONE added v1.3. Clear via CMD_CLEAR_ERROR (v1.3) */
#define RBAMP_V2_REG_VERSION                   0x03u   /* 0x01=v1.0 .. 0x04=v1.3 */
#define RBAMP_V2_REG_MODE                      0x04u   /* 0=production, 1=develop (PB5 strap at boot) */
#define RBAMP_V2_REG_CT_MODEL                  0x05u   /* SCT-013 SKU 0=unset/1=-005/2=-010/3=-030/4=-050/5=-100/6=-020/7=-060 (v1.3). Direct write applies preset to ch */
#define RBAMP_V2_REG_V03_PHASE_SAMPLES         0x06u   /* U-vs-I sample advance, 0..30. Develop-gated write (v1.3). Save via CMD_SAVE_GAINS. */
#define RBAMP_V2_REG_V03_PERIOD_VALID          0x07u   /* Set by CMD_LATCH_PERIOD: 1=fresh snapshot, 0=empty accumulator (race). NOT cleared-on-read. Failed latch does  */
#define RBAMP_V2_REG_LUT_VALID_MASK            0x08u   /* bit n = slot n has valid LUT */
#define RBAMP_V2_REG_LUT_QUERY_SLOT            0x09u   /* Select slot 0..3 → metadata latched into 0x0A-0x0F */
#define RBAMP_V2_REG_LUT_VIEW_TIER             0x0Au   /* 0=BASIC, 1=STANDARD */
#define RBAMP_V2_REG_LUT_VIEW_POINTS_LOG2      0x0Bu   /* 8 or 9 */
#define RBAMP_V2_REG_LUT_VIEW_INL_MAX          0x0Cu   /* Measured INL_max */
#define RBAMP_V2_REG_LUT_VIEW_INL_MAX_SIZE     2
#define RBAMP_V2_REG_LUT_VIEW_DNL_MAX          0x0Eu   /* Measured DNL_max */
#define RBAMP_V2_REG_LUT_VIEW_DNL_MAX_SIZE     2
#define RBAMP_V2_REG_ADC_MEAN_U                0x10u   /* Raw ADC mean of U channel (~2048 centered). DC-offset cal: gate |mean-2048|<tol at 0A */
#define RBAMP_V2_REG_ADC_MEAN_U_SIZE           2
#define RBAMP_V2_REG_ADC_MEAN_I0               0x12u   /* Raw ADC mean of I0 channel */
#define RBAMP_V2_REG_ADC_MEAN_I0_SIZE          2
#define RBAMP_V2_REG_ADC_MEAN_I1               0x14u   /* Raw ADC mean of I1 (UI2/UI3/I2/I3) */
#define RBAMP_V2_REG_ADC_MEAN_I1_SIZE          2
#define RBAMP_V2_REG_ADC_MEAN_I2               0x16u   /* Raw ADC mean of I2 (UI3/I3) */
#define RBAMP_V2_REG_ADC_MEAN_I2_SIZE          2
#define RBAMP_V2_REG_CAPTURE_STATUS            0x18u   /* v1.3 raw-capture diag (major-carry glitch): bit0=ready. Arm via CMD_CAPTURE_RAW */
#define RBAMP_V2_REG_CAPTURE_PAGE              0x19u   /* Page 0..7 — latches 32 raw I0 samples into CAPTURE_WINDOW */
#define RBAMP_V2_REG_CAPTURE_WINDOW            0x1Au   /* 32×u16 LE raw pre-LUT I0 codes of selected page. Burst-read 64 bytes. 8 pages × 32 = 256 samples ~1.3 mains pe */
#define RBAMP_V2_REG_CAPTURE_WINDOW_SIZE       64
#define RBAMP_V2_REG_AC_FREQ                   0x20u   /* 50 or 60 */
#define RBAMP_V2_REG_AC_PERIOD                 0x21u   /* Mains half-period */
#define RBAMP_V2_REG_AC_PERIOD_SIZE            2
#define RBAMP_V2_REG_CALIBRATION               0x23u   /* Legacy calibration status byte */
#define RBAMP_V2_REG_TOPOLOGY                  0x24u   /* 1=SINGLE, 2=SPLIT_PHASE, 3=THREE_PHASE (=V03_N_I) */
#define RBAMP_V2_REG_SENSOR_CLASS              0x25u   /* 0=UNSET, 1=SCT_013, 2=WIRED_CT, 3=BUILTIN_CT. Class change resets CT_MODEL=0. */
#define RBAMP_V2_REG_V03_PHASE_FRACT           0x26u   /* Sub-sample phase shift Q8. Develop-gated write (v1.3). Save via CMD_SAVE_GAINS. */
#define RBAMP_V2_REG_FLEET_CONFIG              0x27u   /* bit0=GC_ENABLE (General-Call latch reception; effective after reset - ENGC not toggled live). bits1-7 reserved */
#define RBAMP_V2_REG_GROUP_ID                  0x28u   /* GC latch group filter. 0 = respond to all-call only. GC frame group byte must match or be 0x00 */
#define RBAMP_V2_REG_DIGEST_CONFIG             0x29u   /* Digest window composition bitmask (see digest_mask_bits). Bits unsupported by variant → ERR_PARAM. 0 = digest  */
#define RBAMP_V2_REG_EVENT_FLAGS               0x2Au   /* Sticky event bits, write-1-to-clear (see event_bits). DRDY held solid LOW while (EVENT_FLAGS & EVENT_MASK) !=  */
#define RBAMP_V2_REG_EVENT_MASK                0x2Bu   /* Which EVENT_FLAGS bits assert DRDY solid LOW (alarm class). 0 = line never held */
#define RBAMP_V2_REG_THRESH_I_HI               0x2Cu   /* Current threshold → EVENT_FLAGS.THRESH_I. 0xFFFF = disabled. Applies to max(I_rms[ch]) */
#define RBAMP_V2_REG_THRESH_I_HI_SIZE          2
#define RBAMP_V2_REG_THRESH_P_HI               0x2Eu   /* Power threshold → EVENT_FLAGS.THRESH_P. 0xFFFF = disabled. Applies to sum(P[ch]) */
#define RBAMP_V2_REG_THRESH_P_HI_SIZE          2
#define RBAMP_V2_REG_I2C_ADDRESS               0x30u   /* v1.3 two-phase: write candidate (0x08..0x77) -> RAM only (reads return staged value); arm ADDR_COMMIT_MAGIC th */
#define RBAMP_V2_REG_ADDR_COMMIT_MAGIC         0x31u   /* Write 0xA5 to arm CMD_COMMIT_ADDR; consumed (cleared) on commit attempt. Write-only - reads return 0x00 */
#define RBAMP_V2_REG_UPTIME_S                  0x46u   /* Seconds since boot */
#define RBAMP_V2_REG_UPTIME_S_SIZE             4
#define RBAMP_V2_REG_RESET_CAUSE               0x4Au   /* Last reset reason flags from RCC_CSR: bit0=PIN, bit1=POR/BOR, bit2=SW, bit3=IWDG, bit4=WWDG, bit5=LPWR */
#define RBAMP_V2_REG_I2C_ERR_COUNT             0x4Bu   /* Accumulated bus errors (BERR+OVR) since boot, saturating */
#define RBAMP_V2_REG_I2C_ERR_COUNT_SIZE        2
#define RBAMP_V2_REG_I2C_REINIT_COUNT          0x4Du   /* I2C peripheral BUSY-recovery reinit count, saturating */
#define RBAMP_V2_REG_ZC_OFFSET                 0x4Eu   /* Time from last GC-latch STOP edge to next voltage zero-cross. U-variants only (CAPABILITY bit); I-variants rea */
#define RBAMP_V2_REG_ZC_OFFSET_SIZE            2
#define RBAMP_V2_REG_CT_MODEL_CH0              0x51u   /* v1.3 D-1.3: CT model actually APPLIED to channel 0 (0=unset). Mixed-CT modules: per-channel assignment persist */
#define RBAMP_V2_REG_CT_MODEL_CH1              0x52u   /* Model applied to channel 1 */
#define RBAMP_V2_REG_CT_MODEL_CH2              0x53u   /* Model applied to channel 2 */
#define RBAMP_V2_REG_PRODUCT_ID                0x54u   /* Product family: 0x01=rbAmp sensor, 0x02=rbDimmer (own map!). Master MUST read before interpreting family-speci */
#define RBAMP_V2_REG_HW_VARIANT                0x55u   /* BUILD_VARIANT: 1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3 */
#define RBAMP_V2_REG_FW_TIER                   0x56u   /* bits0-1: 0=BASIC,1=STANDARD,2=PRO; bit2=bidirectional; bit3=LUT-calibrated */
#define RBAMP_V2_REG_CAPABILITY                0x57u   /* Feature bitmap (see capability_bits). Libraries branch on bits, never on VERSION heuristics */
#define RBAMP_V2_REG_CAPABILITY_SIZE           2
#define RBAMP_V2_REG_GC_TICK                   0x59u   /* Master tick from last accepted GC-latch frame; 0xFFFF = never received. Fleet-wide window numbering + per-modu */
#define RBAMP_V2_REG_GC_TICK_SIZE              2
#define RBAMP_V2_REG_UID                       0x5Cu   /* 96-bit chip UID (3×u32 LE from UID_BASE). One burst read. Used by: address arbitration, seal verification, sti */
#define RBAMP_V2_REG_UID_SIZE                  12
#define RBAMP_V2_REG_LABEL                     0x68u   /* User location label, ASCII zero-padded ('boiler'). Empty = unset → replacement-detection signal */
#define RBAMP_V2_REG_LABEL_SIZE                8
#define RBAMP_V2_REG_DIGEST                    0x70u   /* Compact poll window, one burst read. Layout: [STATUS_MIRROR u8][SEQ u8] then fields in canonical order, only m */
#define RBAMP_V2_REG_DIGEST_SIZE               22
#define RBAMP_V2_REG_V03_U_RMS                 0x86u   /* 0.0 on I-variants */
#define RBAMP_V2_REG_V03_U_RMS_SIZE            4
#define RBAMP_V2_REG_V03_U_PEAK                0x8Au
#define RBAMP_V2_REG_V03_U_PEAK_SIZE           4
#define RBAMP_V2_REG_V03_I0_RMS                0x8Eu
#define RBAMP_V2_REG_V03_I0_RMS_SIZE           4
#define RBAMP_V2_REG_V03_I1_RMS                0x92u   /* 0.0 if variant lacks ch1 */
#define RBAMP_V2_REG_V03_I1_RMS_SIZE           4
#define RBAMP_V2_REG_V03_I2_RMS                0x96u   /* 0.0 if variant lacks ch2 */
#define RBAMP_V2_REG_V03_I2_RMS_SIZE           4
#define RBAMP_V2_REG_V03_I0_PEAK               0x9Au
#define RBAMP_V2_REG_V03_I0_PEAK_SIZE          4
#define RBAMP_V2_REG_V03_I1_PEAK               0x9Eu
#define RBAMP_V2_REG_V03_I1_PEAK_SIZE          4
#define RBAMP_V2_REG_V03_I2_PEAK               0xA2u
#define RBAMP_V2_REG_V03_I2_PEAK_SIZE          4
#define RBAMP_V2_REG_V03_P0_REAL               0xA6u   /* 0.0 on I-variants (no power calc) */
#define RBAMP_V2_REG_V03_P0_REAL_SIZE          4
#define RBAMP_V2_REG_V03_P1_REAL               0xAAu
#define RBAMP_V2_REG_V03_P1_REAL_SIZE          4
#define RBAMP_V2_REG_V03_P2_REAL               0xAEu
#define RBAMP_V2_REG_V03_P2_REAL_SIZE          4
#define RBAMP_V2_REG_V03_PF0                   0xB2u   /* -1..+1 */
#define RBAMP_V2_REG_V03_PF0_SIZE              4
#define RBAMP_V2_REG_V03_PF1                   0xB6u
#define RBAMP_V2_REG_V03_PF1_SIZE              4
#define RBAMP_V2_REG_V03_PF2                   0xBAu
#define RBAMP_V2_REG_V03_PF2_SIZE              4
#define RBAMP_V2_REG_V03_PERIOD_COMMIT_CNT     0xBEu   /* RT commits within current period (diagnostic) */
#define RBAMP_V2_REG_V03_PERIOD_COMMIT_CNT_SIZE 4
#define RBAMP_V2_REG_V03_PERIOD_AVG_P_CH1      0xC2u   /* Latched avg P ch1 (UI2/UI3) */
#define RBAMP_V2_REG_V03_PERIOD_AVG_P_CH1_SIZE 4
#define RBAMP_V2_REG_V03_PERIOD_AVG_P_CH2      0xC6u   /* Latched avg P ch2 (UI3) */
#define RBAMP_V2_REG_V03_PERIOD_AVG_P_CH2_SIZE 4
#define RBAMP_V2_REG_V03_PERIOD_MS             0xCAu   /* Current period duration */
#define RBAMP_V2_REG_V03_PERIOD_MS_SIZE        4
#define RBAMP_V2_REG_V03_STATUS                0xCEu   /* bit0=valid (RT commit result). NOT cleared-on-read. Libraries use STATUS 0x00 for ready-wait */
#define RBAMP_V2_REG_V03_RESERVED_CF           0xCFu   /* Reserved, reads 0x00 */
#define RBAMP_V2_REG_V03_Q0_REAC               0xD0u   /* Reactive power ch0 (IEEE 1459 quadrature) */
#define RBAMP_V2_REG_V03_Q0_REAC_SIZE          4
#define RBAMP_V2_REG_V03_Q1_REAC               0xD4u
#define RBAMP_V2_REG_V03_Q1_REAC_SIZE          4
#define RBAMP_V2_REG_V03_Q2_REAC               0xD8u
#define RBAMP_V2_REG_V03_Q2_REAC_SIZE          4
#define RBAMP_V2_REG_V03_PERIOD_AVG_P          0xDCu   /* PRODUCTION energy primitive: latched avg P ch0, >=0 (BASIC unidirectional clamp) */
#define RBAMP_V2_REG_V03_PERIOD_AVG_P_SIZE     4
#define RBAMP_V2_REG_V03_PERIOD_MAX_P          0xE0u   /* Latched max P ch0 this period */
#define RBAMP_V2_REG_V03_PERIOD_MAX_P_SIZE     4
#define RBAMP_V2_REG_V03_U_NOISE_FLOOR         0xE4u   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_U_NOISE_FLOOR_SIZE    2
#define RBAMP_V2_REG_V03_I0_NOISE_FLOOR        0xE6u   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I0_NOISE_FLOOR_SIZE   2
#define RBAMP_V2_REG_V03_I1_NOISE_FLOOR        0xE8u   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I1_NOISE_FLOOR_SIZE   2
#define RBAMP_V2_REG_V03_I2_NOISE_FLOOR        0xEAu   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I2_NOISE_FLOOR_SIZE   2
#define RBAMP_V2_REG_V03_PERIOD_LATCH_MS       0xECu   /* Chip-side dt between last two latches. Master fallback after its own restart */
#define RBAMP_V2_REG_V03_PERIOD_LATCH_MS_SIZE  4
#define RBAMP_V2_REG_V03_U_GAIN                0xF0u   /* Develop-gated write (v1.3). Save via CMD_SAVE_GAINS */
#define RBAMP_V2_REG_V03_U_GAIN_SIZE           4
#define RBAMP_V2_REG_V03_I0_GAIN               0xF4u   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I0_GAIN_SIZE          4
#define RBAMP_V2_REG_V03_I1_GAIN               0xF8u   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I1_GAIN_SIZE          4
#define RBAMP_V2_REG_V03_I2_GAIN               0xFCu   /* Develop-gated write (v1.3) */
#define RBAMP_V2_REG_V03_I2_GAIN_SIZE          4

/* ---- Command opcodes ---- */
#define RBAMP_V2_CMD_NOP                       0x00u
#define RBAMP_V2_CMD_RESET                     0x01u
#define RBAMP_V2_CMD_RECALIBRATE               0x02u
#define RBAMP_V2_CMD_SWITCH_UART               0x03u
#define RBAMP_V2_CMD_CAL_BEGIN                 0x20u
#define RBAMP_V2_CMD_CAL_SAMPLE                0x21u
#define RBAMP_V2_CMD_CAL_LUT_WRITE             0x22u
#define RBAMP_V2_CMD_CAL_LUT_COMMIT            0x23u
#define RBAMP_V2_CMD_CAL_LUT_ABORT             0x24u
#define RBAMP_V2_CMD_CAL_END                   0x25u
#define RBAMP_V2_CMD_SAVE_GAINS                0x26u
#define RBAMP_V2_CMD_LATCH_PERIOD              0x27u
#define RBAMP_V2_CMD_SET_CT_MODEL_CH0          0x28u
#define RBAMP_V2_CMD_SET_CT_MODEL_CH1          0x29u
#define RBAMP_V2_CMD_SET_CT_MODEL_CH2          0x2Au
#define RBAMP_V2_CMD_COMMIT_ADDR               0x30u
#define RBAMP_V2_CMD_CLEAR_ERROR               0x31u
#define RBAMP_V2_CMD_SAVE_USER_CONFIG          0x32u
#define RBAMP_V2_CMD_SEAL                      0x33u
#define RBAMP_V2_CMD_UID_ARBITRATE             0x34u
#define RBAMP_V2_CMD_UID_PRESENT               0x35u
#define RBAMP_V2_CMD_UID_MUTE_RESET            0x36u
#define RBAMP_V2_CMD_ENTER_BOOTLOADER          0x37u
#define RBAMP_V2_CMD_CAPTURE_RAW               0x38u
#define RBAMP_V2_CMD_FACTORY_RESET             0xAAu

/* ---- Command settle times (ms) ---- */
#define RBAMP_V2_SETTLE_MS_NOP                 0u
#define RBAMP_V2_SETTLE_MS_RESET               300u
#define RBAMP_V2_SETTLE_MS_RECALIBRATE         200u
#define RBAMP_V2_SETTLE_MS_SWITCH_UART         50u
#define RBAMP_V2_SETTLE_MS_CAL_BEGIN           10u
#define RBAMP_V2_SETTLE_MS_CAL_SAMPLE          50u
#define RBAMP_V2_SETTLE_MS_CAL_LUT_WRITE       5u
#define RBAMP_V2_SETTLE_MS_CAL_LUT_COMMIT      700u
#define RBAMP_V2_SETTLE_MS_CAL_LUT_ABORT       5u
#define RBAMP_V2_SETTLE_MS_CAL_END             50u
#define RBAMP_V2_SETTLE_MS_SAVE_GAINS          700u
#define RBAMP_V2_SETTLE_MS_LATCH_PERIOD        50u
#define RBAMP_V2_SETTLE_MS_SET_CT_MODEL_CH0    5u
#define RBAMP_V2_SETTLE_MS_SET_CT_MODEL_CH1    5u
#define RBAMP_V2_SETTLE_MS_SET_CT_MODEL_CH2    5u
#define RBAMP_V2_SETTLE_MS_COMMIT_ADDR         700u
#define RBAMP_V2_SETTLE_MS_CLEAR_ERROR         0u
#define RBAMP_V2_SETTLE_MS_SAVE_USER_CONFIG    700u
#define RBAMP_V2_SETTLE_MS_SEAL                700u
#define RBAMP_V2_SETTLE_MS_UID_ARBITRATE       5u
#define RBAMP_V2_SETTLE_MS_UID_PRESENT         10u
#define RBAMP_V2_SETTLE_MS_UID_MUTE_RESET      10u
#define RBAMP_V2_SETTLE_MS_ENTER_BOOTLOADER    100u
#define RBAMP_V2_SETTLE_MS_CAPTURE_RAW         80u
#define RBAMP_V2_SETTLE_MS_FACTORY_RESET       1500u

/* ---- Device error codes ---- */
#define RBAMP_V2_DEV_ERR_OK                    0x00u
#define RBAMP_V2_DEV_ERR_CLONE                 0xF9u
#define RBAMP_V2_DEV_ERR_LUT_BAD               0xFAu
#define RBAMP_V2_DEV_ERR_FLASH_PARAMS_BAD      0xFBu
#define RBAMP_V2_DEV_ERR_NOT_READY             0xFCu
#define RBAMP_V2_DEV_ERR_SENSOR_OVERFLOW       0xFDu
#define RBAMP_V2_DEV_ERR_PARAM                 0xFEu
#define RBAMP_V2_DEV_ERR_UNHANDLED             0xFFu

/* ---- Library error codes ---- */
#define RBAMP_V2_LIB_OK                        (0)
#define RBAMP_V2_LIB_ERR_IO                    (-1)
#define RBAMP_V2_LIB_ERR_NACK                  (-2)
#define RBAMP_V2_LIB_ERR_TIMEOUT               (-3)
#define RBAMP_V2_LIB_ERR_NOT_READY             (-4)
#define RBAMP_V2_LIB_ERR_STALE                 (-5)
#define RBAMP_V2_LIB_ERR_PARAM                 (-6)
#define RBAMP_V2_LIB_ERR_MODE                  (-7)
#define RBAMP_V2_LIB_ERR_CHECKSUM              (-8)
#define RBAMP_V2_LIB_ERR_VERSION               (-9)
#define RBAMP_V2_LIB_ERR_NOT_IMPLEMENTED       (-10)
#define RBAMP_V2_LIB_ERR_NON_PHYSICAL          (-11)

/* ---- CAPABILITY register (0x57) bits ---- */
#define RBAMP_V2_CAP_EXT_ADDRESSING            (1u << 0)
#define RBAMP_V2_CAP_GC_LATCH                  (1u << 1)
#define RBAMP_V2_CAP_GC_GROUP_FILTER           (1u << 2)
#define RBAMP_V2_CAP_DIGEST                    (1u << 3)
#define RBAMP_V2_CAP_EVENTS                    (1u << 4)
#define RBAMP_V2_CAP_UID_ARBITRATION           (1u << 5)
#define RBAMP_V2_CAP_SEAL                      (1u << 6)
#define RBAMP_V2_CAP_TWO_PHASE_ADDR            (1u << 7)
#define RBAMP_V2_CAP_ZC_PHASE_OFFSET           (1u << 8)
#define RBAMP_V2_CAP_SAVE_USER_CONFIG          (1u << 9)
#define RBAMP_V2_CAP_CLEAR_ERROR               (1u << 10)
#define RBAMP_V2_CAP_IAP                       (1u << 11)

/* ---- DIGEST_CONFIG (0x29) mask bits ---- */
#define RBAMP_V2_DIGEST_I_RMS                  (1u << 0)
#define RBAMP_V2_DIGEST_U_RMS                  (1u << 1)
#define RBAMP_V2_DIGEST_P_REAL                 (1u << 2)
#define RBAMP_V2_DIGEST_PF                     (1u << 3)

/* ---- EVENT_FLAGS (0x2A) / EVENT_MASK (0x2B) bits ---- */
#define RBAMP_V2_EVENT_PERIOD_READY            (1u << 0)
#define RBAMP_V2_EVENT_THRESH_I                (1u << 1)
#define RBAMP_V2_EVENT_THRESH_P                (1u << 2)
#define RBAMP_V2_EVENT_ERROR                   (1u << 3)
#define RBAMP_V2_EVENT_CONFIG_CHANGED          (1u << 4)
#define RBAMP_V2_EVENT_RESET_OCCURRED          (1u << 5)

/* ---- Extended address space (0xFF-prefix, 16-bit) — reserved layout ----
 *   0x0100-0x011F: Bidirectional: PERIOD_AVG_P_NEG[3] f32, E_NEG accumulators (decision 5.3: F4 tiers only)
 *   0x0120-0x01FF: Channels 3..7 (UI5/UI7): RT float block mirroring 0x86 layout
 *   0x0200-0x02FF: IAP/bootloader control block (F4)
 *   0x0300-0xFFFF: reserved
 */

#ifdef __cplusplus
}
#endif

#endif /* RBAMP_REGISTERS_V2_H */
