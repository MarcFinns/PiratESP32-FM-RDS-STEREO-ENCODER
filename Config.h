/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                          Audio Configuration Header
 *
 * =====================================================================================
 *
 * File:         Config.h
 * Description:  Centralized configuration for audio DSP pipeline
 *
 * Purpose:
 *   This header contains all configurable parameters for the stereo encoder.
 *   Modify these values to adapt the system to different hardware or requirements.
 *
 * Configuration Categories:
 *   • GPIO Pin Assignments
 *   • Sample Rates and Timing
 *   • DSP Algorithm Parameters
 *   • FM Stereo MPX Levels
 *   • Diagnostic Options
 *   • VU Meter Display Settings
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>

// ==================================================================================
//                           DIAGNOSTIC CONFIGURATION
// ==================================================================================

/**
 * Diagnostic Print Interval
 *
 * Controls how often detailed diagnostic information is printed to serial.
 * Set to 0 to disable diagnostic printing (recommended for production).
 *
 * Values:
 *   0     = Disabled (no diagnostic output, best performance)
 *   1-N   = Print diagnostics every N blocks (increases Serial traffic)
 *
 * Note: Performance logging (every 5 seconds) is independent and always enabled.
 */
#define DIAGNOSTIC_PRINT_INTERVAL 0

namespace Config
{
    // ==================================================================================
    //                              GPIO PIN ASSIGNMENTS
    // ==================================================================================
    //
    // Pin Configuration for I2S Audio Interfaces
    //
    // Hardware Setup:
    //   • MCLK (24.576 MHz) is shared between ADC and DAC
    //   • ADC operates at 48 kHz (MCLK ÷ 512 = 48 kHz)
    //   • DAC operates at 192 kHz (MCLK ÷ 128 = 192 kHz)
    //   • Both use standard I2S protocol (Philips format)
    //
    // Pin Usage:
    //   - BCK (Bit Clock): Carries serial audio data clock
    //   - LRCK (Left/Right Clock): Word select (L/R channel indicator)
    //   - DOUT/DIN: Serial audio data (24-bit samples)
    //   - MCLK: Master clock for ADC/DAC synchronization

    // ---- Master Clock (Shared) ----
    constexpr int PIN_MCLK = 8; // 24.576 MHz MCLK output (shared by ADC and DAC)

    // ---- DAC Interface (I2S TX @ 192 kHz) ----
    constexpr int PIN_DAC_BCK  = 9;  // DAC bit clock (BCK)
    constexpr int PIN_DAC_LRCK = 11; // DAC word select (LRCK / WS)
    constexpr int PIN_DAC_DOUT = 10; // DAC serial data output

    // ---- ADC Interface (I2S RX @ 48 kHz) ----
    constexpr int PIN_ADC_BCK  = 4; // ADC bit clock (BCK)
    constexpr int PIN_ADC_LRCK = 6; // ADC word select (LRCK / WS)
    constexpr int PIN_ADC_DIN  = 5; // ADC serial data input

    // ==================================================================================
    //                          SAMPLE RATES AND TIMING
    // ==================================================================================

    /**
     * Input Sample Rate (ADC)
     *
     * The rate at which audio samples are captured from the I2S ADC.
     * Standard CD-quality rate, optimal for speech and music.
     *
     * Value: 48,000 Hz (48 kHz)
     */
    constexpr uint32_t SAMPLE_RATE_ADC = 48000;

    /**
     * Output Sample Rate (DAC)
     *
     * The rate at which processed audio is output to the I2S DAC.
     * 4x higher than input to accommodate FM multiplex bandwidth.
     *
     * Value: 192,000 Hz (192 kHz)
     */
    constexpr uint32_t SAMPLE_RATE_DAC = 192000;

    /**
     * Upsampling Factor
     *
     * Ratio between output and input sample rates: DAC / ADC = 192k / 48k = 4
     * This determines the polyphase FIR filter configuration.
     *
     * Value: 4x upsampling
     */
    constexpr std::size_t UPSAMPLE_FACTOR = 4;

    /**
     * Audio Block Size
     *
     * Number of stereo sample pairs processed per iteration.
     * Smaller blocks reduce latency but increase overhead.
     * Larger blocks improve efficiency but increase latency.
     *
     * Current: 64 samples = 1.33 ms @ 48 kHz
     *          (64 samples ÷ 48,000 samples/sec = 0.00133 sec)
     */
    constexpr std::size_t BLOCK_SIZE = 64;

    /**
     * Bit Depth Configuration
     *
     * I2S audio format: 24-bit samples in 32-bit words (MSB-aligned)
     *   • BITS_PER_SAMPLE: Actual audio precision (24 bits)
     *   • BYTES_PER_SAMPLE: Memory allocation per sample (4 bytes = 32 bits)
     */
    constexpr std::size_t BITS_PER_SAMPLE  = 24; // 24-bit audio precision
    constexpr std::size_t BYTES_PER_SAMPLE = 4;  // 32-bit word (4 bytes)

    /**
     * Fixed-Point to Float Conversion Factor
     *
     * Used to convert 32-bit signed integers to normalized float [-1.0, +1.0]
     * Q31 format: int32 ÷ 2^31 = float
     *
     * Value: 2^31 = 2,147,483,648
     */
    constexpr float Q31_FULL_SCALE = 2147483648.0f;

    // ==================================================================================
    //                         PRE-EMPHASIS FILTER PARAMETERS
    // ==================================================================================
    //
    // FM Broadcasting Standard: Pre-emphasis compensates for high-frequency noise
    // by boosting treble before transmission. The receiver applies de-emphasis
    // to restore flat response while reducing noise.
    //
    // Standard: 50 µs time constant (Europe) or 75 µs (North America)

    /**
     * Pre-emphasis Time Constant
     *
     * Determines the corner frequency of the high-pass pre-emphasis filter.
     * European FM broadcasting standard.
     *
     * Formula: fc = 1 / (2π × τ) = 1 / (2π × 50µs) ≈ 3.18 kHz
     *
     * Value: 50 microseconds (EU standard)
     */
    constexpr float PREEMPHASIS_TIME_CONSTANT_US = 50.0f;

    /**
     * Pre-emphasis Filter Coefficient (Alpha)
     *
     * First-order IIR high-pass filter coefficient calculated from time constant.
     * Derived from: α = exp(-1 / (τ × fs))
     *
     * At 192 kHz: α = exp(-1 / (50e-6 × 192000)) ≈ 0.6592
     *
     * Value: 0.6592 (dimensionless)
     */
    constexpr float PREEMPHASIS_ALPHA = 0.6592f;

    /**
     * Pre-emphasis Output Gain
     *
     * Compensates for the RMS level reduction caused by high-pass filtering.
     * Ensures consistent modulation depth across frequency spectrum.
     *
     * Value: 3.0x gain (empirically tuned for FM stereo)
     */
    constexpr float PREEMPHASIS_GAIN = 3.0f;

    // ==================================================================================
    //                         19 kHz NOTCH FILTER PARAMETERS
    // ==================================================================================
    //
    // Purpose: Remove any 19 kHz content from the audio signal to prevent
    // interference with the FM stereo pilot tone (which is exactly 19 kHz).

    /**
     * Notch Filter Center Frequency
     *
     * Targets the FM stereo pilot tone frequency.
     * Must precisely match the pilot tone to avoid interference.
     *
     * Value: 19,000 Hz (19 kHz)
     */
    constexpr float NOTCH_FREQUENCY_HZ = 19000.0f;

    /**
     * Notch Filter Radius (Q Factor)
     *
     * Controls the width and depth of the notch.
     *   • Higher values (closer to 1.0): Narrower notch, deeper attenuation
     *   • Lower values: Wider notch, gentler attenuation
     *
     * Current: 0.98 provides ~-40 dB attenuation at 19 kHz with minimal impact
     * on adjacent frequencies (18-20 kHz).
     *
     * Value: 0.98 (dimensionless, typical range: 0.90-0.99)
     */
    constexpr float NOTCH_RADIUS = 0.98f;

    // ==================================================================================
    //                          FM STEREO MPX LEVELS
    // ==================================================================================
    //
    // FM Multiplex Signal Composition:
    //   • Mono (L+R): 0-15 kHz, full bandwidth
    //   • Pilot tone: 19 kHz, 9% modulation
    //   • Stereo subcarrier: 23-53 kHz (38 kHz ± 15 kHz), DSB-SC with L-R

    /**
     * Feature toggles for MPX components
     *
     * ENABLE_AUDIO: Includes program audio into the composite (mono and L-R)
     * ENABLE_PILOT: Includes the 19 kHz pilot tone
     * ENABLE_RDS:   Includes the 57 kHz RDS subcarrier
     * ENABLE_SUBCARRIER_38K: Enables the 38 kHz stereo subcarrier (L−R DSB)
     */
    constexpr bool ENABLE_AUDIO          = true;
    constexpr bool ENABLE_PILOT          = true;
    constexpr bool ENABLE_RDS            = true;
    constexpr bool ENABLE_SUBCARRIER_38K = true;

    // (Subcarrier phase offset removed; NCO now uses sine basis with zero-cross alignment)

    /**
     * Pilot Tone Amplitude
     *
     * The 19 kHz pilot tone indicates stereo transmission to the receiver.
     * FM standard: 9% ± 1% of total modulation.
     *
     * Value: 0.09 (9% modulation depth)
     */
    constexpr float PILOT_AMP = 0.09f;

    /**
     * Stereo Subcarrier Amplitude (L-R)
     *
     * The difference signal (L-R) is modulated onto a 38 kHz suppressed carrier.
     * Amplitude is chosen to maintain proper stereo separation without overmodulation.
     *
     * Value: 0.90 (90% of available modulation depth)
     */
    /**
     * Stereo difference (L-R) injection.
     * Set to 1.0 so decoded L/R at the receiver have correct unity gain
     * when combined with the unscaled L+R path. Reduce if composite peaks
     * approach clipping, or add composite limiting.
     */
    constexpr float DIFF_AMP = 1.0f;

    // ==================================================================================
    //                          CARRIER TEST OUTPUT (DEBUG)
    // ==================================================================================
    /**
     * TEST_OUTPUT_CARRIERS:
     *   When true, bypass normal MPX generation and output:
     *     Left DAC  = 19 kHz pilot (sine)
     *     Right DAC = 38 kHz subcarrier (sine)
     *   Useful to verify phase relationship on an oscilloscope.
     */
    constexpr bool TEST_OUTPUT_CARRIERS = false;

    /** Amplitude for test carriers (0.0..1.0). */
    constexpr float TEST_CARRIER_AMP = 0.80f;

    // ==================================================================================
    //                          PERFORMANCE MONITORING
    // ==================================================================================

    /**
     * Statistics Print Interval
     *
     * How often performance statistics are logged to Serial.
     * Includes: CPU usage, processing time, headroom, peak levels.
     *
     * Value: 5,000,000 microseconds (5 seconds)
     */
    constexpr uint64_t STATS_PRINT_INTERVAL_US = 5000000ULL;

    // ==================================================================================
    //                             LEVEL TAPS (DIAGNOSTICS)
    // ==================================================================================
    // Optional lightweight peak logging at key pipeline stages to analyze headroom
    // and potential clipping. Disabled by default. Non‑destructive and easy to remove.

    /** Enable/disable level taps logging. */
    constexpr bool LEVEL_TAPS_ENABLE = true;

    /**
     * Level taps logging interval.
     * How often to print peak levels summary. Default matches perf logs (5 s).
     */
    constexpr uint64_t LEVEL_TAPS_INTERVAL_US = 5000000ULL;

    // ==================================================================================
    //                          VU METER CONFIGURATION
    // ==================================================================================

    /**
     * VU Update Interval
     *
     * How often the DSP_pipeline sends VU sample data to the display task.
     * Faster updates provide more responsive meters but increase queue traffic.
     *
     * Current: 5 ms = 200 Hz update rate
     *          This is much faster than the display refresh (50 Hz), ensuring
     *          no audio peaks are missed.
     *
     * Value: 5,000 microseconds (5 ms)
     */
    constexpr uint64_t VU_UPDATE_INTERVAL_US = 5000ULL;

    /**
     * dBFS Reference Level
     *
     * Full-scale reference for dBFS calculations.
     * In float audio (range -1.0 to +1.0), full scale = 1.0
     *
     * Formula: dBFS = 20 × log₁₀(sample / DBFS_REF)
     *
     * Value: 1.0 (normalized float full scale)
     */
    constexpr float DBFS_REF = 1.0f;

    // ==================================================================================
    //                       ILI9341 TFT DISPLAY CONFIGURATION
    // ==================================================================================

    /**
     * Enable/Disable VU Display
     *
     * Set to false to disable TFT display initialization and rendering.
     * Useful for testing audio pipeline without display hardware.
     *
     * Value: true (display enabled)
     */
    constexpr bool VU_DISPLAY_ENABLED = true;

    // ---- SPI Pin Assignments for ILI9341 ----
    //
    // Standard SPI interface with separate DC (Data/Command) pin.
    // Uses hardware SPI (VSPI on ESP32) for maximum transfer speed.

    constexpr int TFT_SCK  = 40; // SPI clock (SCLK)
    constexpr int TFT_MOSI = 41; // SPI data out (MOSI / SDI)
    constexpr int TFT_DC   = 42; // Data/Command select (DC / RS)
    constexpr int TFT_CS   = 1;  // Chip select (CS)
    constexpr int TFT_RST  = 2;  // Hardware reset (RST)

    /**
     * TFT Backlight Control Pin
     *
     * GPIO pin for controlling display backlight.
     * Set to -1 if backlight is hard-wired to power (always on).
     *
     * Value: -1 (backlight always on)
     */
    constexpr int TFT_BL = -1;

    /**
     * Display Rotation
     *
     * ILI9341 orientation setting:
     *   0 = Portrait (240×320)
     *   1 = Landscape (320×240) ← Current setting
     *   2 = Portrait flipped
     *   3 = Landscape flipped
     *
     * Value: 1 (landscape mode for horizontal VU bars)
     */
    constexpr int TFT_ROTATION = 1;

    // ==================================================================================
    //                          VU METER CALIBRATION
    // ==================================================================================

    /**
     * VU Meter dB Offset
     *
     * Shifts the entire dB scale to adjust VU meter sensitivity.
     * Applied to measured dBFS values before display mapping.
     *
     * Use Cases:
     *   • Compensate for digital headroom in audio pipeline
     *   • Adjust meter sensitivity for different source levels
     *   • Make quieter signals more visible on the display
     *
     * Effect on Display:
     *   Offset = 0.0:  True dBFS scale (-40 to +3 dB)
     *                  Only loud signals move the meters
     *
     *   Offset = 10.0: Scale shifts up by 10 dB
     *                  -30 dBFS input shows as -20 dB (more sensitive)
     *
     *   Offset = -10.0: Scale shifts down by 10 dB
     *                   -30 dBFS input shows as -40 dB (less sensitive)
     *
     * Typical Values:
     *   0.0  = No adjustment (default, accurate dBFS)
     *   5.0  = Moderate boost (good for moderate signals)
     *   10.0 = High boost (good for quiet signals)
     *
     * Value: 0.0 dB (no offset, true dBFS scale)
     */
    constexpr float VU_DB_OFFSET = 0.0f;

    /**
     * VU Meter Mode Selection
     *
     * Determines which audio metric drives the VU bar length:
     *
     *   true  = Peak mode (recommended)
     *           • Shows instantaneous peak levels
     *           • Responds immediately to transients
     *           • Better for monitoring clipping
     *           • Current setting
     *
     *   false = RMS mode
     *           • Shows average power (RMS = Root Mean Square)
     *           • Smoother, less responsive to brief peaks
     *           • Better represents perceived loudness
     *           • May miss brief clipping events
     *
     * Value: true (peak mode)
     */
    constexpr bool VU_USE_PEAK_FOR_BAR = true;

    // ==================================================================================
    //                          ON-SCREEN STATUS PANEL
    // ==================================================================================
    /**
     * Bottom status panel height (pixels). The VU bars are positioned above this
     * area when the display is enabled.
     */
    constexpr int STATUS_PANEL_HEIGHT = 112;

    /**
     * Status update interval sent from the audio task to the VU display task.
     * Expressed in microseconds.
     */
    constexpr uint64_t STATUS_PANEL_UPDATE_US = 1000000ULL; // 1 second

    // ==================================================================================
    //                          RDS (RADIO DATA SYSTEM)
    // ==================================================================================

    /** RDS injection amplitude (fraction of full-scale MPX). Typical 0.02–0.04 */
    constexpr float RDS_AMP = 0.03f;

    /** RDS symbol rate (RDS bps) */
    constexpr float RDS_SYMBOL_RATE = 1187.5f;

} // namespace Config

// =====================================================================================
//                                END OF FILE
// =====================================================================================
