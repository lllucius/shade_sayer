/**
 * @file tcs3530.h
 * @brief TCS3530 Register Map, Bitfields, Reset Values, and Helper Functions
 *
 * This header provides:
 *  - Complete register addresses for the TCS3530 true-color XYZ sensor.
 *  - Reset values as given in the AMS datasheet.
 *  - Key bit masks and positions.
 *  - Simple configuration "profiles" for common modes.
 *  - Inline helper functions to compute and program timing and thresholds.
 *
 * @note All registers are 8-bit.  Multi-byte quantities are little-endian
 *       (low byte at lower address).
 * @note Timing is derived from an internal ~720 kHz oscillator and a
 *       "modulator clock" period of ~1.388889 µs.
 *
 * @warning LATCHING RULE (from datasheet): For 16-bit and 24-bit fields,
 *          the LOW BYTE MUST BE ACCESSED FIRST.
 *          - Reads:  read low → latches entire field, then read high.
 *          - Writes: write low, then write high.
 *
 * The caller must provide low-level I²C primitives:
 * @code
 *   tcs3530_write8(user_ctx, reg, val)
 *   tcs3530_read8(user_ctx, reg, &val)
 * @endcode
 */

#ifndef TCS3530_REGS_H
#define TCS3530_REGS_H

// --------------------------------------------------------------------------
// I2C ADDRESS
// --------------------------------------------------------------------------

// 7-bit I^2C address of TCS3530 (fixed)
#define TCS3530_I2C_ADDR                     0x39u

// --------------------------------------------------------------------------
// REGISTER ADDRESSES + RESET VALUES (from datasheet register map)
// --------------------------------------------------------------------------

// 0x24 CONTROL_SCL: software reset
#define TCS3530_REG_CONTROL_SCL              0x24u
#define TCS3530_REG_CONTROL_SCL_RESET        0x00u

// 0x40-0x4F MOD_OFFSETx: 10-bit 2's-complement offset per modulator
#define TCS3530_REG_MOD_OFFSET0_L            0x40u
#define TCS3530_REG_MOD_OFFSET0_H            0x41u
#define TCS3530_REG_MOD_OFFSET1_L            0x42u
#define TCS3530_REG_MOD_OFFSET1_H            0x43u
#define TCS3530_REG_MOD_OFFSET2_L            0x44u
#define TCS3530_REG_MOD_OFFSET2_H            0x45u
#define TCS3530_REG_MOD_OFFSET3_L            0x46u
#define TCS3530_REG_MOD_OFFSET3_H            0x47u
#define TCS3530_REG_MOD_OFFSET4_L            0x48u
#define TCS3530_REG_MOD_OFFSET4_H            0x49u
#define TCS3530_REG_MOD_OFFSET5_L            0x4Au
#define TCS3530_REG_MOD_OFFSET5_H            0x4Bu
#define TCS3530_REG_MOD_OFFSET6_L            0x4Cu
#define TCS3530_REG_MOD_OFFSET6_H            0x4Du
#define TCS3530_REG_MOD_OFFSET7_L            0x4Eu
#define TCS3530_REG_MOD_OFFSET7_H            0x4Fu

#define TCS3530_REG_MOD_OFFSETx_L_RESET      0x00u
#define TCS3530_REG_MOD_OFFSETx_H_RESET      0x00u

// 0x7F OSCEN: oscillator and power-on polling
#define TCS3530_REG_OSCEN                    0x7Fu
#define TCS3530_REG_OSCEN_RESET              0x00u

// 0x80 ENABLE: master enables for ALS and Flicker engines
#define TCS3530_REG_ENABLE                   0x80u
#define TCS3530_REG_ENABLE_RESET             0x00u

// 0x81 MEAS_MODE0: general measurement behavior, ALS scaling
#define TCS3530_REG_MEAS_MODE0               0x81u
#define TCS3530_REG_MEAS_MODE0_RESET         0x04u

// 0x82 MEAS_MODE1: ALS MSB position, optional flicker info to FIFO
#define TCS3530_REG_MEAS_MODE1               0x82u
#define TCS3530_REG_MEAS_MODE1_RESET         0x0Cu

// 0x83-0x84 SAMPLE_TIME: base sample period (flicker + ALS step)
#define TCS3530_REG_SAMPLE_TIME0             0x83u
#define TCS3530_REG_SAMPLE_TIME1             0x84u
#define TCS3530_REG_SAMPLE_TIME0_RESET       0x60u
#define TCS3530_REG_SAMPLE_TIME1_RESET       0x16u

// 0x85-0x86 SAMPLE_TIME_ALTERNATIVE: optional 2nd sample time set
#define TCS3530_REG_SAMPLE_TIME_ALT0         0x85u
#define TCS3530_REG_SAMPLE_TIME_ALT1         0x86u
#define TCS3530_REG_SAMPLE_TIME_ALT0_RESET   0x60u
#define TCS3530_REG_SAMPLE_TIME_ALT1_RESET   0x16u

// 0x87-0x88 ALS_NR_SAMPLES: ALS integration length (in sample steps)
#define TCS3530_REG_ALS_NR_SAMPLES0          0x87u
#define TCS3530_REG_ALS_NR_SAMPLES1          0x88u
#define TCS3530_REG_ALS_NR_SAMPLES0_RESET    0x00u
#define TCS3530_REG_ALS_NR_SAMPLES1_RESET    0x00u

// 0x89-0x8A ALS_NR_SAMPLES_ALTERNATIVE
#define TCS3530_REG_ALS_NR_SAMPLES_ALT0      0x89u
#define TCS3530_REG_ALS_NR_SAMPLES_ALT1      0x8Au
#define TCS3530_REG_ALS_NR_SAMPLES_ALT0_RESET 0x00u
#define TCS3530_REG_ALS_NR_SAMPLES_ALT1_RESET 0x00u

// 0x8B-0x8C FD_NR_SAMPLES: flicker integration length
#define TCS3530_REG_FD_NR_SAMPLES0           0x8Bu
#define TCS3530_REG_FD_NR_SAMPLES1           0x8Cu
#define TCS3530_REG_FD_NR_SAMPLES0_RESET     0x00u
#define TCS3530_REG_FD_NR_SAMPLES1_RESET     0x00u

// 0x8D-0x8E FD_NR_SAMPLES_ALTERNATIVE
#define TCS3530_REG_FD_NR_SAMPLES_ALT0       0x8Du
#define TCS3530_REG_FD_NR_SAMPLES_ALT1       0x8Eu
#define TCS3530_REG_FD_NR_SAMPLES_ALT0_RESET 0x00u
#define TCS3530_REG_FD_NR_SAMPLES_ALT1_RESET 0x00u

// 0x8F WTIME: wait time between sequencer / modulator measurements
#define TCS3530_REG_WTIME                    0x8Fu
#define TCS3530_REG_WTIME_RESET              0x00u

// 0x90-0x92: IDs
#define TCS3530_REG_AUX_ID                   0x90u
#define TCS3530_REG_REV_ID                   0x91u
#define TCS3530_REG_ID                       0x92u
#define TCS3530_REG_AUX_ID_RESET             0x00u
#define TCS3530_REG_REV_ID_RESET             0x14u
#define TCS3530_REG_ID_RESET                 0x68u

// 0x93-0x95: ALS low threshold (24-bit)
#define TCS3530_REG_AILT0                    0x93u
#define TCS3530_REG_AILT1                    0x94u
#define TCS3530_REG_AILT2                    0x95u
#define TCS3530_REG_AILT0_RESET              0x00u
#define TCS3530_REG_AILT1_RESET              0x00u
#define TCS3530_REG_AILT2_RESET              0x00u

// 0x96-0x98: ALS high threshold (24-bit)
#define TCS3530_REG_AIHT0                    0x96u
#define TCS3530_REG_AIHT1                    0x97u
#define TCS3530_REG_AIHT2                    0x98u
#define TCS3530_REG_AIHT0_RESET              0x00u
#define TCS3530_REG_AIHT1_RESET              0x00u
#define TCS3530_REG_AIHT2_RESET              0x00u

// 0x99-0x9A AGC_NR_SAMPLES: number of samples used for AGC
#define TCS3530_REG_AGC_NR_SAMPLES0          0x99u
#define TCS3530_REG_AGC_NR_SAMPLES1          0x9Au
#define TCS3530_REG_AGC_NR_SAMPLES0_RESET    0x00u
#define TCS3530_REG_AGC_NR_SAMPLES1_RESET    0x00u

// 0x9B-0xA0 STATUSx: interrupt and saturation flags
#define TCS3530_REG_STATUS                   0x9Bu
#define TCS3530_REG_STATUS2                  0x9Cu
#define TCS3530_REG_STATUS3                  0x9Du
#define TCS3530_REG_STATUS4                  0x9Eu
#define TCS3530_REG_STATUS5                  0x9Fu
#define TCS3530_REG_STATUS6                  0xA0u
#define TCS3530_REG_STATUS_RESET             0x00u
#define TCS3530_REG_STATUS2_RESET            0x00u
#define TCS3530_REG_STATUS3_RESET            0x08u
#define TCS3530_REG_STATUS4_RESET            0x00u
#define TCS3530_REG_STATUS5_RESET            0x00u
#define TCS3530_REG_STATUS6_RESET            0x00u

// 0xA1-0xAA CFGx: configuration group
#define TCS3530_REG_CFG0                     0xA1u
#define TCS3530_REG_CFG1                     0xA2u
#define TCS3530_REG_CFG2                     0xA3u
#define TCS3530_REG_CFG3                     0xA4u
#define TCS3530_REG_CFG4                     0xA5u
#define TCS3530_REG_CFG5                     0xA6u
#define TCS3530_REG_CFG6                     0xA7u
#define TCS3530_REG_CFG7                     0xA8u
#define TCS3530_REG_CFG8                     0xA9u
#define TCS3530_REG_CFG9                     0xAAu
#define TCS3530_REG_CFG0_RESET               0x00u
#define TCS3530_REG_CFG1_RESET               0x00u
#define TCS3530_REG_CFG2_RESET               0x00u
#define TCS3530_REG_CFG3_RESET               0x00u
#define TCS3530_REG_CFG4_RESET               0x00u
#define TCS3530_REG_CFG5_RESET               0x00u
#define TCS3530_REG_CFG6_RESET               0x03u
#define TCS3530_REG_CFG7_RESET               0x01u
#define TCS3530_REG_CFG8_RESET               0xC3u
#define TCS3530_REG_CFG9_RESET               0x00u

// 0xAB MOD_CHANNEL_CTRL: enable/disable individual modulators
#define TCS3530_REG_MOD_CHANNEL_CTRL         0xABu
#define TCS3530_REG_MOD_CHANNEL_CTRL_RESET   0x00u

// 0xAD TRIGGER_MODE: periodic triggering via timer or VSYNC
#define TCS3530_REG_TRIGGER_MODE             0xADu
#define TCS3530_REG_TRIGGER_MODE_RESET       0x00u

// 0xAE OSC_TUNE: manual oscillator trim (normally keep at 0)
#define TCS3530_REG_OSC_TUNE                 0xAEu
#define TCS3530_REG_OSC_TUNE_RESET           0x00u

// 0xB0 VSYNC_GPIO_INT: INT and VSYNC/GPIO pin behavior
#define TCS3530_REG_VSYNC_GPIO_INT           0xB0u
#define TCS3530_REG_VSYNC_GPIO_INT_RESET     0x02u
#define TCS3530_GPIO_OUTPUT_LEVEL_BIT        (1u << 1)  // Bit 1: GPIO output level
#define TCS3530_GPIO_INPUT_ENABLE_BIT        (1u << 2)  // Bit 2: GPIO input enable

// 0xBA INTENAB: enables for external INT pin events
#define TCS3530_REG_INTENAB                  0xBAu
#define TCS3530_REG_INTENAB_RESET            0x00u

// 0xBB SIEN: system interrupt (SINT) enables
#define TCS3530_REG_SIEN                     0xBBu
#define TCS3530_REG_SIEN_RESET               0x00u

// 0xBC CONTROL: clearing FIFO and SAI_ACTIVE
#define TCS3530_REG_CONTROL                  0xBCu
#define TCS3530_REG_CONTROL_RESET            0x00u

// 0xBD ALS_DATA_STATUS: indicates fresh ALS data is available
#define TCS3530_REG_ALS_DATA_STATUS          0xBDu
#define TCS3530_REG_ALS_DATA_STATUS_RESET    0x00u

// 0xBE ALS_DATA_FIRST: coherence buffer first-byte indicator
#define TCS3530_REG_ALS_DATA_FIRST           0xBEu
#define TCS3530_REG_ALS_DATA_FIRST_RESET     0x00u

// 0xBF ALS_DATA: streaming window into ALS data / FIFO
#define TCS3530_REG_ALS_DATA                 0xBFu
#define TCS3530_REG_ALS_DATA_RESET           0x00u

// 0xC0-0xCF MEAS_SEQR_STEPx_MOD_GAINX_y: per-step modulator gains
#define TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_0 0xC0u
#define TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_1 0xC1u
#define TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_2 0xC2u
#define TCS3530_REG_MEAS_SEQR_STEP0_MOD_GAINX_3 0xC3u
#define TCS3530_REG_MEAS_SEQR_STEP1_MOD_GAINX_0 0xC4u
#define TCS3530_REG_MEAS_SEQR_STEP1_MOD_GAINX_1 0xC5u
#define TCS3530_REG_MEAS_SEQR_STEP1_MOD_GAINX_2 0xC6u
#define TCS3530_REG_MEAS_SEQR_STEP1_MOD_GAINX_3 0xC7u
#define TCS3530_REG_MEAS_SEQR_STEP2_MOD_GAINX_0 0xC8u
#define TCS3530_REG_MEAS_SEQR_STEP2_MOD_GAINX_1 0xC9u
#define TCS3530_REG_MEAS_SEQR_STEP2_MOD_GAINX_2 0xCAu
#define TCS3530_REG_MEAS_SEQR_STEP2_MOD_GAINX_3 0xCBu
#define TCS3530_REG_MEAS_SEQR_STEP3_MOD_GAINX_0 0xCCu
#define TCS3530_REG_MEAS_SEQR_STEP3_MOD_GAINX_1 0xCDu
#define TCS3530_REG_MEAS_SEQR_STEP3_MOD_GAINX_2 0xCEu
#define TCS3530_REG_MEAS_SEQR_STEP3_MOD_GAINX_3 0xCFu

#define TCS3530_REG_MEAS_SEQR_STEPX_MOD_GAINX_y_RESET 0x88u // default all modulators gain nibble = 0x8

// 0xD0-0xD3: per-step flicker detection pattern bits (mod0..7)
#define TCS3530_REG_MEAS_SEQR_STEP0_FD       0xD0u
#define TCS3530_REG_MEAS_SEQR_STEP1_FD       0xD1u
#define TCS3530_REG_MEAS_SEQR_STEP2_FD       0xD2u
#define TCS3530_REG_MEAS_SEQR_STEP3_FD       0xD3u
#define TCS3530_REG_MEAS_SEQR_STEP0_FD_RESET 0x00u
#define TCS3530_REG_MEAS_SEQR_STEP1_FD_RESET 0x00u
#define TCS3530_REG_MEAS_SEQR_STEP2_FD_RESET 0x00u
#define TCS3530_REG_MEAS_SEQR_STEP3_FD_RESET 0x00u

// 0xD4-0xD7: per-step residual measurement enable bits
#define TCS3530_REG_MEAS_SEQR_STEP0_RESIDUAL 0xD4u
#define TCS3530_REG_MEAS_SEQR_STEP1_RESIDUAL 0xD5u
#define TCS3530_REG_MEAS_SEQR_STEP2_RESIDUAL 0xD6u
#define TCS3530_REG_MEAS_SEQR_STEP3_RESIDUAL 0xD7u
#define TCS3530_REG_MEAS_SEQR_STEP0_RESIDUAL_RESET 0xFFu
#define TCS3530_REG_MEAS_SEQR_STEP1_RESIDUAL_RESET 0xFFu
#define TCS3530_REG_MEAS_SEQR_STEP2_RESIDUAL_RESET 0xFFu
#define TCS3530_REG_MEAS_SEQR_STEP3_RESIDUAL_RESET 0xFFu

// 0xD8-0xDB: per-step ALS measurement enable bits
#define TCS3530_REG_MEAS_SEQR_STEP0_ALS      0xD8u
#define TCS3530_REG_MEAS_SEQR_STEP1_ALS      0xD9u
#define TCS3530_REG_MEAS_SEQR_STEP2_ALS      0xDAu
#define TCS3530_REG_MEAS_SEQR_STEP3_ALS      0xDBu
#define TCS3530_REG_MEAS_SEQR_STEP0_ALS_RESET 0xFFu
#define TCS3530_REG_MEAS_SEQR_STEP1_ALS_RESET 0x00u
#define TCS3530_REG_MEAS_SEQR_STEP2_ALS_RESET 0x00u
#define TCS3530_REG_MEAS_SEQR_STEP3_ALS_RESET 0x00u

// 0xDC MEAS_SEQR_APERS_AND_VSYNC_WAIT:
/*
 *  - VSYNC_WAIT_PATTERN[7:4]: which steps wait for VSYNC.
 *  - APERS_PATTERN[3:0]: which steps evaluate ALS persistence.
 */
#define TCS3530_REG_MEAS_SEQR_APERS_AND_VSYNC_WAIT 0xDCu
#define TCS3530_REG_MEAS_SEQR_APERS_AND_VSYNC_WAIT_RESET 0x01u // default: APERS on step 0

// 0xDD MEAS_SEQR_AGC: AGC predict and ASAT enable patterns
#define TCS3530_REG_MEAS_SEQR_AGC            0xDDu
#define TCS3530_REG_MEAS_SEQR_AGC_RESET      0xFFu // enabled on all steps

// 0xDE MEAS_SEQR_SMUX_AND_SAMPLE_TIME:
/*
 *  - SAMPLE_TIME_PATTERN[7:4]: choose main vs alt SAMPLE_TIME per step.
 *  - SMUX_PATTERN[3:0]: choose SMUX mapping variant per step.
 */
#define TCS3530_REG_MEAS_SEQR_SMUX_AND_SAMPLE_TIME 0xDEu
#define TCS3530_REG_MEAS_SEQR_SMUX_AND_SAMPLE_TIME_RESET 0x00u

// 0xDF MEAS_SEQR_WAIT_AND_TS_ENABLE: step wait mask
#define TCS3530_REG_MEAS_SEQR_WAIT_AND_TS_ENABLE 0xDFu
#define TCS3530_REG_MEAS_SEQR_WAIT_AND_TS_ENABLE_RESET 0x01u // default: wait after last step

// 0xE0 MOD_CALIB_CFG0: calibration repetition rate
#define TCS3530_REG_MOD_CALIB_CFG0           0xE0u
#define TCS3530_REG_MOD_CALIB_CFG0_RESET     0xFFu // "only once at start"

// 0xE2 MOD_CALIB_CFG2: residual/AZ/AGC calibration options
#define TCS3530_REG_MOD_CALIB_CFG2           0xE2u
#define TCS3530_REG_MOD_CALIB_CFG2_RESET     0x69u

// 0xE3 MOD_CALIB_CFG3: autozero settling time and iterations
#define TCS3530_REG_MOD_CALIB_CFG3           0xE3u
#define TCS3530_REG_MOD_CALIB_CFG3_RESET     0x6Au

// 0xE7 MOD_COMP_CFG2: IDAC range selection
#define TCS3530_REG_MOD_COMP_CFG2            0xE7u
#define TCS3530_REG_MOD_COMP_CFG2_RESET      0xBFu

// 0xE8-0xEA MOD_RESIDUAL_CFGx: residual timing scaling
#define TCS3530_REG_MOD_RESIDUAL_CFG0        0xE8u
#define TCS3530_REG_MOD_RESIDUAL_CFG1        0xE9u
#define TCS3530_REG_MOD_RESIDUAL_CFG2        0xEAu
#define TCS3530_REG_MOD_RESIDUAL_CFG0_RESET  0x00u
#define TCS3530_REG_MOD_RESIDUAL_CFG1_RESET  0x00u
#define TCS3530_REG_MOD_RESIDUAL_CFG2_RESET  0x00u

// 0xEB-0xEC VSYNC_DELAY_CFGx: offset between VSYNC and sampling
#define TCS3530_REG_VSYNC_DELAY_CFG0         0xEBu
#define TCS3530_REG_VSYNC_DELAY_CFG1         0xECu
#define TCS3530_REG_VSYNC_DELAY_CFG0_RESET   0x00u
#define TCS3530_REG_VSYNC_DELAY_CFG1_RESET   0x00u

// 0xED-0xEE VSYNC_PERIODx: measured VSYNC period (read-only)
#define TCS3530_REG_VSYNC_PERIOD0            0xEDu
#define TCS3530_REG_VSYNC_PERIOD1            0xEEu
#define TCS3530_REG_VSYNC_PERIOD0_RESET      0x00u
#define TCS3530_REG_VSYNC_PERIOD1_RESET      0x00u

// 0xEF-0xF0 VSYNC_PERIOD_TARGETx: expected VSYNC period (for sync check)
#define TCS3530_REG_VSYNC_PERIOD_TARGET0     0xEFu
#define TCS3530_REG_VSYNC_PERIOD_TARGET1     0xF0u
#define TCS3530_REG_VSYNC_PERIOD_TARGET0_RESET 0x00u
#define TCS3530_REG_VSYNC_PERIOD_TARGET1_RESET 0x00u

// 0xF1 VSYNC_CONTROL: software-driven VSYNC pulses
#define TCS3530_REG_VSYNC_CONTROL            0xF1u
#define TCS3530_REG_VSYNC_CONTROL_RESET      0x00u

// 0xF2 VSYNC_CFG: oscillator calibration and VSYNC source
#define TCS3530_REG_VSYNC_CFG                0xF2u
#define TCS3530_REG_VSYNC_CFG_RESET          0x00u

// 0xF3 FIFO_THR: FIFO level at which FINT is raised (upper bits)
#define TCS3530_REG_FIFO_THR                 0xF3u
#define TCS3530_REG_FIFO_THR_RESET           0x7Fu

// 0xF4-0xFB MOD_FIFO_DATA_CFGx:
/*
 *  - selects which modulator's ALS/flicker data go to FIFO
 *  - sets compression mode and width.
 */
#define TCS3530_REG_MOD_FIFO_DATA_CFG0       0xF4u
#define TCS3530_REG_MOD_FIFO_DATA_CFG1       0xF5u
#define TCS3530_REG_MOD_FIFO_DATA_CFG2       0xF6u
#define TCS3530_REG_MOD_FIFO_DATA_CFG3       0xF7u
#define TCS3530_REG_MOD_FIFO_DATA_CFG4       0xF8u
#define TCS3530_REG_MOD_FIFO_DATA_CFG5       0xF9u
#define TCS3530_REG_MOD_FIFO_DATA_CFG6       0xFAu
#define TCS3530_REG_MOD_FIFO_DATA_CFG7       0xFBu
#define TCS3530_REG_MOD_FIFO_DATA_CFGx_RESET 0x8Fu

// 0xFC-0xFD FIFO_STATUSx: level, overflow/underflow flags
#define TCS3530_REG_FIFO_STATUS0             0xFCu
#define TCS3530_REG_FIFO_STATUS1             0xFDu
#define TCS3530_REG_FIFO_STATUS0_RESET       0x00u
#define TCS3530_REG_FIFO_STATUS1_RESET       0x00u

// 0xFE FIFO_DATA_PROTOCOL: alternative FIFO read method
#define TCS3530_REG_FIFO_DATA_PROTOCOL       0xFEu
#define TCS3530_REG_FIFO_DATA_PROTOCOL_RESET 0x00u

// 0xFF FIFO_DATA: primary FIFO data read register
#define TCS3530_REG_FIFO_DATA                0xFFu
#define TCS3530_REG_FIFO_DATA_RESET          0x00u

// --------------------------------------------------------------------------
/*
 * KEY BITFIELDS - EXPLANATORY COMMENTS
 * (Only the most commonly used ones are expanded; you can add more as needed.)
 * -------------------------------------------------------------------------- */

// CONTROL_SCL (0x24)
/*
 *  bit0 SOFT_RESET: write 1 to perform a software reset (equivalent to POR).
 */
#define TCS3530_CONTROL_SCL_SOFT_RESET_Pos   0u
#define TCS3530_CONTROL_SCL_SOFT_RESET_Msk   (1u << 0)

// OSCEN (0x7F)
/*
 *  bit2 PON_INIT: read-only, 1 during ~300 us after power-up; device NACKs.
 *  bit1 OSCEN_STATUS: read-only, 1 if oscillator is running.
 *  bit0 OSCEN: R/W, 1 to enable oscillator, 0 to disable. PON uses this.
 */
#define TCS3530_OSCEN_PON_INIT_Pos           2u
#define TCS3530_OSCEN_PON_INIT_Msk           (1u << 2)
#define TCS3530_OSCEN_OSCEN_STATUS_Pos       1u
#define TCS3530_OSCEN_OSCEN_STATUS_Msk       (1u << 1)
#define TCS3530_OSCEN_OSCEN_Pos              0u
#define TCS3530_OSCEN_OSCEN_Msk              (1u << 0)

// ENABLE (0x80)
/*
 *  bit6 FDEN:  Flicker detection engine enable.
 *  bit1 AEN:   ALS/Color engine enable.
 *  bit0 PON:   Power-on: start oscillator & state machine.
 *
 * Typical bring-up sequence:
 *  1) configure all non-volatile settings (thresholds, timing).
 *  2) set PON=1.
 *  3) then enable AEN and/or FDEN.
 */
#define TCS3530_ENABLE_FDEN_Pos              6u
#define TCS3530_ENABLE_FDEN_Msk              (1u << 6)
#define TCS3530_ENABLE_AEN_Pos               1u
#define TCS3530_ENABLE_AEN_Msk               (1u << 1)
#define TCS3530_ENABLE_PON_Pos               0u
#define TCS3530_ENABLE_PON_Msk               (1u << 0)

// MEAS_MODE0 (0x81)
/*
 *  bit7 STOP_AFTER_NTH_ITERATION:
 *      When set, measurement stops after Nth calibration iteration, clearing
 *      FDEN and AEN but leaving PON=1.
 *  bit6 ENABLE_AGC_ASAT_DOUBLE_STEP_DOWN:
 *      If analog saturation occurs, AGC may lower gain by 2 steps at once.
 *  bit5 MEASUREMENT_SEQUENCER_SINGLE_SHOT_MODE:
 *      When set, one sequencer round is executed then SAI can sleep the device.
 *  bit4 MOD_FIFO_ALS_STATUS_WRITE_ENABLE:
 *      If using ALS data compression into FIFO, also push ALS status.
 *  bits3:0 ALS_SCALE:
 *      Number of most-significant ALS bits that must be zero before data is
 *      "scaled" into 16-bit ALS_data. Helps avoid clipping high bits away.
 */
#define TCS3530_MEAS_MODE0_STOP_AFTER_NTH_ITERATION_Pos 7u
#define TCS3530_MEAS_MODE0_STOP_AFTER_NTH_ITERATION_Msk (1u << 7)
#define TCS3530_MEAS_MODE0_ENABLE_AGC_ASAT_DOUBLE_STEP_DOWN_Pos 6u
#define TCS3530_MEAS_MODE0_ENABLE_AGC_ASAT_DOUBLE_STEP_DOWN_Msk (1u << 6)
#define TCS3530_MEAS_MODE0_MEAS_SEQR_SINGLE_SHOT_Pos 5u
#define TCS3530_MEAS_MODE0_MEAS_SEQR_SINGLE_SHOT_Msk (1u << 5)
#define TCS3530_MEAS_MODE0_MOD_FIFO_ALS_STATUS_WRITE_ENABLE_Pos 4u
#define TCS3530_MEAS_MODE0_MOD_FIFO_ALS_STATUS_WRITE_ENABLE_Msk (1u << 4)
#define TCS3530_MEAS_MODE0_ALS_SCALE_Pos     0u
#define TCS3530_MEAS_MODE0_ALS_SCALE_Msk     0x0Fu

// MEAS_MODE1 (0x82)
/*
 *  bit7 MOD_FIFO_FD_END_MARKER_WRITE_ENABLE:
 *      Write end marker after each flicker measurement sequence into FIFO.
 *  bit6 MOD_FIFO_FD_CHECKSUM_WRITE_ENABLE:
 *      Also push flicker checksum.
 *  bit5 MOD_FIFO_FD_GAIN_WRITE_ENABLE:
 *      Push flicker gain to FIFO (needed when AGC changes gain).
 *  bits4:0 ALS_MSB_POSITION:
 *      MSB index in a virtual 32-bit ALS accumulator. Controls where to align
 *      ALS results when compressing to 16 bits.
 */
#define TCS3530_MEAS_MODE1_ALS_MSB_POSITION_Pos 0u
#define TCS3530_MEAS_MODE1_ALS_MSB_POSITION_Msk 0x1Fu

// SAMPLE_TIME0/1 (0x83-0x84)
/*
 *  SAMPLE_TIME is 11 bits total:
 *      SAMPLE_TIME = [SAMPLE_TIME1(10:3), SAMPLE_TIME0(2:0)]
 *  Step period:
 *      Tstep = (SAMPLE_TIME + 1) * 1.388889 us
 *
 *  For default reset (0x60, 0x16 => 179): Tstep ~ 250 us.
 *
 *  SAMPLE_TIME0:
 *      bits7:5 SAMPLE_TIME[2:0] (LSBs)
 *      bits3:0 MEAS_SEQUENCER_FD_NR_SAMPLES_PATTERN:
 *          Select FD_NR_SAMPLES vs FD_NR_SAMPLES_ALTERNATIVE per step.
 */
#define TCS3530_SAMPLE_TIME0_SAMPLE_TIME_LSB_Pos 5u
#define TCS3530_SAMPLE_TIME0_SAMPLE_TIME_LSB_Msk (0x07u << 5)

// Additional bit masks for SAMPLE_TIME register manipulation
#define TCS3530_SAMPLE_TIME_LSB_Msk          0x07u  // Bits 2:0 for SAMPLE_TIME0[7:5]
#define TCS3530_SAMPLE_TIME_MSB_Msk          0xFFu  // Bits 10:3 in SAMPLE_TIME1

// ALS_NR_SAMPLES0/1 (0x87-0x88)
/*
 *  ALS_NR_SAMPLES is 11 bits:
 *      ALS_NR_SAMPLES = [ALS_NR_SAMPLES1(10:8), ALS_NR_SAMPLES0(7:0)]
 *  ALS integration time:
 *      T_als = (ALS_NR_SAMPLES + 1) * (SAMPLE_TIME + 1) * 1.388889 us
 */
#define TCS3530_ALS_NR_SAMPLES0_LSB_Pos      0u
#define TCS3530_ALS_NR_SAMPLES0_LSB_Msk      0xFFu
#define TCS3530_ALS_NR_SAMPLES1_MSB_Pos      0u
#define TCS3530_ALS_NR_SAMPLES1_MSB_Msk      0x07u

// FD_NR_SAMPLES1 (0x8C)
/*
 *  bit7 FD_NR_SAMPLES_INFINITE: continuous flicker measurement, no end markers.
 */
#define TCS3530_FD_NR_SAMPLES1_INFINITE_Pos  7u
#define TCS3530_FD_NR_SAMPLES1_INFINITE_Msk  (1u << 7)

// STATUS (0x9B)
/*
 *  bit7 MINT:   "Modulator interrupt" (e.g. analog or digital saturation).
 *  bit3 AINT:   ALS interrupt (thresholds).
 *  bit2 FINT:   FIFO level interrupt.
 *  bit0 SINT:   System interrupt (vsync, sequencer, aux).
 *  Writing '1' to each bit clears that interrupt.
 */
#define TCS3530_STATUS_MINT_Msk              (1u << 7)
#define TCS3530_STATUS_AINT_Msk              (1u << 3)
#define TCS3530_STATUS_FINT_Msk              (1u << 2)
#define TCS3530_STATUS_SINT_Msk              (1u << 0)

// STATUS2 (0x9C)
/*
 *  bit4 ASAT_DIGITAL:  ALS digital saturation detected (ADC overflow)
 *  bit0 ASAT_ANALOG_ANY: Any ALS modulator reports analog saturation
 */
#define TCS3530_STATUS2_ASAT_DIGITAL_Pos      4u
#define TCS3530_STATUS2_ASAT_DIGITAL_Msk      (1u << 4)
#define TCS3530_STATUS2_ASAT_ANALOG_ANY_Pos   0u
#define TCS3530_STATUS2_ASAT_ANALOG_ANY_Msk   (1u << 0)

// STATUS6 (0xA0)
/*
 *  bit7:0 ASAT_ANALOG_MOD[7:0]: Per-modulator analog saturation bits.
 *  Non-zero indicates at least one ALS modulator saturated in analog front-end.
 */
#define TCS3530_STATUS6_ASAT_ANALOG_MOD_Msk   0xFFu

// INTENAB (0xBA)
/*
 *  bit7 MIEN:   enable MINT->INT mapping.
 *  bit3 AIEN:   enable AINT->INT mapping.
 *  bit2 FIEN:   enable FINT->INT mapping.
 *  bit0 SIEN:   enable SINT->INT mapping.
 */
#define TCS3530_INTENAB_MIEN_Msk             (1u << 7)
#define TCS3530_INTENAB_AIEN_Msk             (1u << 3)
#define TCS3530_INTENAB_FIEN_Msk             (1u << 2)
#define TCS3530_INTENAB_SIEN_Msk             (1u << 0)

// CONTROL (0xBC)
/*
 *  bit1 FIFO_CLR:         clear FIFO, overflow, underflow, FINT.
 *  bit0 CLEAR_SAI_ACTIVE: clear SAI_ACTIVE and leave sleep mode.
 */
#define TCS3530_CONTROL_FIFO_CLR_Msk         (1u << 1)
#define TCS3530_CONTROL_CLEAR_SAI_ACTIVE_Msk (1u << 0)

// CFG0 (0xA1)
/*
 *  bit6 SAI: Sleep After Interrupt. If asserted, the oscillator is turned
 *      off whenever interrupt is active (low). SAI_ACTIVE is set in this event.
 *      Only works with MEASUREMENT_SEQUENCER_SINT_PER_STEP or SIEN or
 *      SIEN_MEASUREMENT_SEQUENCER enabled.
 */
#define TCS3530_CFG0_SAI_Pos                 6u
#define TCS3530_CFG0_SAI_Msk                 (1u << 6)

// STATUS4 (0x9E)
/*
 *  bit1 SAI_ACTIVE: Indicates that the device is in sleep due to an interrupt.
 *      To exit sleep mode, clear this bit by writing "1" to CLEAR_SAI_ACTIVE.
 */
#define TCS3530_STATUS4_SAI_ACTIVE_Pos       1u
#define TCS3530_STATUS4_SAI_ACTIVE_Msk       (1u << 1)

// STATUS5 (0x9F)
/*
 *  bit1 SINT_MEASUREMENT_SEQUENCER: Set when measurement sequencer completes
 *      a round (or step if MEASUREMENT_SEQUENCER_SINT_PER_STEP is enabled).
 */
#define TCS3530_STATUS5_SINT_MEASUREMENT_SEQUENCER_Pos 1u
#define TCS3530_STATUS5_SINT_MEASUREMENT_SEQUENCER_Msk (1u << 1)

// SIEN (0xBB)
/*
 *  bit2 SIEN_AUX:                    Enable SINT for auxiliary events.
 *  bit1 SIEN_MEASUREMENT_SEQUENCER:  Enable SINT for measurement sequencer events.
 *  bit0 SIEN_VSYNC:                  Enable SINT for VSYNC events.
 */
#define TCS3530_SIEN_AUX_Pos                 2u
#define TCS3530_SIEN_AUX_Msk                 (1u << 2)
#define TCS3530_SIEN_MEASUREMENT_SEQUENCER_Pos 1u
#define TCS3530_SIEN_MEASUREMENT_SEQUENCER_Msk (1u << 1)
#define TCS3530_SIEN_VSYNC_Pos               0u
#define TCS3530_SIEN_VSYNC_Msk               (1u << 0)

// Gain register mask (4-bit gain code per modulator)
#define TCS3530_GAIN_CODE_Msk                0x0Fu

// ALS_DATA_STATUS (0xBD)
/*
 *  bit7 ALS_DATA_VALID: ALS completed at least one cycle
 *                        since last read of ALS_DATA_FIRST or (re)asserting AEN.
 */
#define TCS3530_ALS_DATA_STATUS_VALID_Msk    (1u << 7)

// MOD_CHANNEL_CTRL (0xAB)
/*
 *  1 bit per modulator; '1' disables that modulator.
 *  Use to save power if you don't use all 8 modulators.
 */
#define TCS3530_MOD_CHANNEL_CTRL_MOD7_DISABLE_Msk (1u << 7)
#define TCS3530_MOD_CHANNEL_CTRL_MOD0_DISABLE_Msk (1u << 0)

// --------------------------------------------------------------------------
/*
 * HELPER PACK/UNPACK MACROS FOR MULTI-BIT FIELDS
 * -------------------------------------------------------------------------- */

// Pack 11-bit fields split into low byte + 3 MSB bits
#define TCS3530_PACK_11BIT(msb3, lsb8)  \
    ((((uint16_t)((msb3) & 0x07u)) << 8) | ((uint16_t)(lsb8)))

#define TCS3530_UNPACK_11BIT_LSB(v11)   ((uint8_t)((v11) & 0xFFu))
#define TCS3530_UNPACK_11BIT_MSB(v11)   ((uint8_t)(((v11) >> 8) & 0x07u))

// Pack a 24-bit threshold value AI(L/H)T[23:0] from bytes [2:0]
#define TCS3530_PACK_24BIT(b2, b1, b0)  \
    (((uint32_t)(b2) << 16) | ((uint32_t)(b1) << 8) | (uint32_t)(b0))

// --------------------------------------------------------------------------
/*
 * DATA FORMAT CONFIGURATION
 * -------------------------------------------------------------------------- */

// CFG4.MOD_ALS_FIFO_DATA_FORMAT (bits 1:0)
#define TCS3530_CFG4_MOD_ALS_FIFO_DATA_FORMAT_Pos 0u
#define TCS3530_CFG4_MOD_ALS_FIFO_DATA_FORMAT_Msk 0x03u
#define TCS3530_ALS_FMT_16BIT 0x00u
#define TCS3530_ALS_FMT_24BIT 0x01u
#define TCS3530_ALS_FMT_32BIT 0x03u

// CFG7.ALS_CB_ENABLE (bit 7)
#define TCS3530_CFG7_ALS_CB_ENABLE_Msk (1u << 7)

#endif // TCS3530_REGS_H
