/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
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
//                              FIRMWARE METADATA
// ==================================================================================
/**
 * Firmware Version String
 *
 * Semantic version or tag for the firmware build. Update this when making
 * a release so it is reported in SYST:VERSION? and on the splash screen.
 */
constexpr const char *FIRMWARE_VERSION = "0.1.0";

// ==================================================================================
//                              SPLASH SCREEN SETTINGS
// ==================================================================================
/**
 * Splash image vertical offset (pixels)
 *
 * Controls the Y position where the splash image starts.
 * Set this to trim the vertical placement if the image is shorter than 240 px.
 */
constexpr int SPLASH_TOP_Y = 70;

/**
 * Splash image height (pixels)
 *
 * Set this to the actual height of the LOGO image in splashscreen.h.
 * Width is assumed to be 320 px.
 */
constexpr int SPLASH_HEIGHT = 133;

// ==================================================================================
//                              UI COLOR PALETTE (RGB565)
// ==================================================================================
/**
 * UI color palette for the on-device display (ILI9341 - RGB565).
 * Adjust to taste or theme; values are 16-bit RGB565.
 */
namespace UI
{
constexpr uint16_t COLOR_BG = 0x0000;     // Black
constexpr uint16_t COLOR_TEXT = 0xFFFF;   // White
constexpr uint16_t COLOR_DIM = 0x7BEF;    // Mid gray
constexpr uint16_t COLOR_ACCENT = 0xFD20; // Amber/Orange accent
constexpr uint16_t COLOR_GOOD = 0x07E0;   // Green
constexpr uint16_t COLOR_WARN = 0xFD20;   // Amber/Orange
constexpr uint16_t COLOR_MUTED = 0x4208;  // Dark gray
} // namespace UI

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
constexpr int PIN_DAC_BCK = 9;   // DAC bit clock (BCK)
constexpr int PIN_DAC_LRCK = 11; // DAC word select (LRCK / WS)
constexpr int PIN_DAC_DOUT = 10; // DAC serial data output

// ---- ADC Interface (I2S RX @ 48 kHz) ----
constexpr int PIN_ADC_BCK = 4;  // ADC bit clock (BCK)
constexpr int PIN_ADC_LRCK = 6; // ADC word select (LRCK / WS)
constexpr int PIN_ADC_DIN = 5;  // ADC serial data input

// ==================================================================================
//                          SAMPLE RATES AND TIMING
// ==================================================================================

/**
 * Input Sample Rate (ADC)
 *
 * The rate at which audio samples are captured from the I2S ADC.
 *
 * Value: 48,000 Hz (48 kHz)
 */
constexpr uint32_t SAMPLE_RATE_ADC = 48000;

/**
 * Output Sample Rate (DAC)
 *
 * The rate at which processed audio is output to the I2S DAC.
 * 4x higher than input to accommodate FM multiplex bandwidth (38KHz and 57KHz subcarriers).
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
constexpr std::size_t BITS_PER_SAMPLE = 24; // 24-bit audio precision
constexpr std::size_t BYTES_PER_SAMPLE = 4; // 32-bit word (4 bytes)

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
//                          AUDIO I/O TIMEOUTS (MS)
// ==================================================================================
/**
 * I2S read/write timeouts in milliseconds. Prevents the audio task from blocking
 * indefinitely if a peripheral stalls. Values should exceed a typical block period
 * (≈1.33 ms at 48 kHz, 64 frames), but remain small to preserve real‑time behavior.
 */
constexpr uint32_t I2S_READ_TIMEOUT_MS = 5;  // RX timeout per read
constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 5; // TX timeout per write

// ==================================================================================
//                     FREERTOS TASK ALLOCATION (CORE & PRIORITY)
// ==================================================================================
//
// This section centralizes all task allocation decisions: core assignment and priority.
// Each task has three configuration parameters:
//   • CORE: Which CPU core (0 = audio, 1 = I/O)
//   • PRIORITY: FreeRTOS priority level (0-25, higher = more priority)
//   • STACK_WORDS: Stack memory in 32-bit words
//
// Core Assignment Strategy:
//   Core 0: Audio processing (DSP_pipeline only - highest priority, no interruptions)
//   Core 1: I/O operations (Console, VU Meter, RDS - can block on I/O)
//
// Priority Strategy (FreeRTOS ESP32):
//   Level 6: Audio DSP (real-time, cannot block, highest priority)
//   Level 2: Console (CLI + logs, medium priority)
//   Level 1: VU Meter, RDS (display/RDS generation, low priority, can block)
//   Level 0: Idle task (system)

/** ---- CONSOLE TASK (Serial owner + log draining) ---- */
/** Console: Core Assignment (0 or 1) */
constexpr int CONSOLE_CORE = 1; // Core selection for Console task
/** Console: Task Priority */
constexpr uint32_t CONSOLE_PRIORITY = 2; // Medium priority
/** Console: Stack Size (32-bit words) */
constexpr uint32_t CONSOLE_STACK_WORDS =
    8192; // 32 KB (increase to avoid stack canary on heavy console parsing)
/** Console: Message Queue Capacity */
constexpr std::size_t CONSOLE_QUEUE_LEN = 256; // Larger buffer for verbose init

/** ---- VU METER TASK ---- */
/** VU Meter: Core Assignment (0 or 1) */
constexpr int VU_CORE = 1; // Core selection for VU task
/** VU Meter: Task Priority */
constexpr uint32_t VU_PRIORITY = 1; // Low priority
/** VU Meter: Stack Size (32-bit words) */
constexpr uint32_t VU_STACK_WORDS = 4096; // 16 KB
/** VU Meter: Sample Queue (Mailbox Pattern) */
constexpr std::size_t VU_QUEUE_LEN = 1; // Mailbox: holds only latest

/** ---- RDS ASSEMBLER TASK ---- */
/** RDS Assembler: Core Assignment (0 or 1) */
constexpr int RDS_CORE = 1; // Core selection for RDS assembler
/** RDS Assembler: Task Priority */
constexpr uint32_t RDS_PRIORITY = 1; // Low priority
/** RDS Assembler: Stack Size (32-bit words) */
constexpr uint32_t RDS_STACK_WORDS = 4096; // 16 KB
/** RDS Assembler: Bit Queue Capacity */
constexpr std::size_t RDS_BIT_QUEUE_LEN = 1024; // 1024 bits buffer

/** ---- DSP PIPELINE TASK ---- */
/** DSP Pipeline: Core Assignment (0 or 1) */
constexpr int DSP_CORE = 0; // Core selection for DSP pipeline
/** DSP Pipeline: Task Priority */
constexpr uint32_t DSP_PRIORITY = 6; // Highest priority
/** DSP Pipeline: Stack Size (32-bit words) */
constexpr uint32_t DSP_STACK_WORDS = 12288; // 48 KB (DSP buffers)

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
 * High-pass leaky differentiator coefficient calculated from time constant.
 * Derived from: α = exp(-1 / (τ × fs))
 *
 * At 48 kHz (input domain): α = exp(-1 / (50e-6 × 48000)) ≈ 0.6592
 * Computed at compile time using a 5th‑order series approximation to avoid
 * pulling non-constexpr math into configuration.
 */
constexpr float exp_approx(float x)
{
    // 1 + x + x^2/2! + x^3/3! + x^4/4! + x^5/5!
    return 1.0f +
           x * (1.0f + x * (0.5f + x * (1.0f / 6.0f + x * (1.0f / 24.0f + x * (1.0f / 120.0f)))));
}
constexpr float PREEMPHASIS_ALPHA =
    exp_approx(-1.0f / (PREEMPHASIS_TIME_CONSTANT_US * 1.0e-6f * SAMPLE_RATE_ADC));

/**
 * Pre-emphasis Output Gain
 *
 * Compensates for the RMS level reduction caused by high-pass filtering.
 * Ensures consistent modulation depth across frequency spectrum.
 *
 * A 1.5x gain matches typical FM broadcast standards.
 */
constexpr float PREEMPHASIS_GAIN = 1.5f;

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
 * Feature toggles for MPX components - used for debugging
 *
 * ENABLE_AUDIO: Includes program audio into the composite (mono and L-R)
 * ENABLE_STEREO_PILOT_19K: Includes the 19 kHz pilot tone
 * ENABLE_RDS_57K:   Includes the 57 kHz RDS subcarrier
 * ENABLE_STEREO_SUBCARRIER_38K: Enables the 38 kHz stereo subcarrier (L−R DSB)
 */
constexpr bool ENABLE_AUDIO = true;
constexpr bool ENABLE_STEREO_PILOT_19K = true;
constexpr bool ENABLE_RDS_57K = true;
constexpr bool ENABLE_STEREO_SUBCARRIER_38K = true;

/**
 * Enable/Disable Pre-emphasis
 *
 * For measurement and A/B testing. When false, the pre-emphasis stage is
 * bypassed entirely and audio proceeds unmodified from the ADC domain.
 */
constexpr bool ENABLE_PREEMPHASIS = true;

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
 * Value: 0.90 (90% of available modulation depth) according to
 * https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.450-4-201910-I%21%21PDF-E.pdf
 */

constexpr float DIFF_AMP = 0.9f;

// ==================================================================================
//                       PILOT AUTO-MUTE ON SILENCE
// ==================================================================================
/**
 * Automatically mute 19 kHz pilot after sustained input silence.
 *
 * Purpose: When program audio is silent for a configurable duration, the pilot can
 * be disabled to avoid stereo hiss on receivers. As soon as
 * audio reappears above the threshold, the pilot is re-enabled immediately.
 */
constexpr bool PILOT_MUTE_ON_SILENCE = true;

/**
 * Silence detection threshold (RMS, linear). Default: 0.001 (~ -60 dBFS)
 */
constexpr float SILENCE_RMS_THRESHOLD = 0.002f;

/**
 * Hold time before muting pilot (milliseconds). Default: 3000 ms (3 seconds)
 */
constexpr uint32_t SILENCE_HOLD_MS = 2000u;

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
constexpr uint64_t STATS_PRINT_INTERVAL_US = 5000000ULL; // 600000000ULL; //

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
constexpr uint64_t VU_UPDATE_INTERVAL_US = 10000ULL;

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

constexpr int TFT_SCK = 40;  // SPI clock (SCLK)
constexpr int TFT_MOSI = 41; // SPI data out (MOSI / SDI)
constexpr int TFT_DC = 42;   // Data/Command select (DC / RS)
constexpr int TFT_CS = 1;    // Chip select (CS)
constexpr int TFT_RST = 2;   // Hardware reset (RST)

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

/**
 * Display RDS Status Bar
 *
 * When true, shows a compact one-line bar with PI, PTY and flags (ST/RDS/PIL/TP/TA/MS).
 * Disable if you experience instability with your display driver.
 */
constexpr bool DISPLAY_SHOW_RDS_STATUS_BAR = true;

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
// Note: Increased from 0.03 to 0.04 for better compatibility with older receivers
// (e.g., Denon DRA-365RD 1994). Newer receivers handle lower levels fine.
constexpr float RDS_AMP = 0.04f;

/** RDS symbol rate (RDS bps) */
constexpr float RDS_SYMBOL_RATE = 1187.5f;

// ==================================================================================
//                         DSP CORE BUFFER CONFIGURATION
// ==================================================================================

/**
 * NCO (Numerically Controlled Oscillator) Lookup Table Size
 *
 * The number of entries in the pre-computed sine wave table used for
 * efficient frequency generation. Larger tables reduce quantization error
 * but increase memory usage.
 *
 * Value: 1024 entries (typical for embedded systems)
 */
constexpr std::size_t NCO_TABLE_SIZE = 1024;

/**
 * Temporary Buffer Size for Filter Processing
 *
 * Intermediate buffer size used during notch filter processing.
 * Should match BLOCK_SIZE for single-pass processing.
 *
 * Value: 64 samples (matches BLOCK_SIZE)
 */
constexpr std::size_t TEMP_NOTCH_BUFFER_SIZE = 64;

// ==================================================================================
//                    POLYPHASE FIR UPSAMPLER CONFIGURATION
// ==================================================================================

/**
 * FIR Filter Tap Count
 *
 * Total number of filter coefficients in the polyphase FIR upsampler.
 * Larger tap counts reduce aliasing but increase processing overhead.
 *
 * Value: 96 taps (24 taps per phase × 4 phases)
 */
constexpr std::size_t FIR_TAPS = 96;

/**
 * FIR Taps Per Phase
 *
 * Number of filter coefficients per upsampling phase.
 * Derived from: FIR_TAPS ÷ UPSAMPLE_FACTOR = 96 ÷ 4
 *
 * Value: 24 taps per phase
 */
constexpr std::size_t FIR_TAPS_PER_PHASE = 24;

// ==================================================================================
//                           I2S DRIVER CONFIGURATION
// ==================================================================================

/**
 * I2S Bits Per Sample (Wire Format)
 *
 * The number of bits transmitted per I2S sample.
 * Standard for 24-bit audio: 32-bit words (24-bit data + 8-bit padding)
 *
 * Value: 32 bits
 */
constexpr uint32_t I2S_BITS_PER_SAMPLE = 32;

/**
 * I2S Number of Channels
 *
 * Stereo audio: 2 channels (Left + Right)
 * Affects DMA configuration and data layout.
 *
 * Value: 2 (stereo)
 */
constexpr uint32_t I2S_CHANNELS = 2;

/**
 * I2S MCLK Multiple for TX (DAC Output)
 *
 * Master clock divider for DAC output clock generation.
 * MCLK = Sample_Rate × MCLK_Multiple_TX
 * At 192 kHz: 192000 × 128 = 24.576 MHz MCLK
 *
 * Value: 128
 */
constexpr uint32_t I2S_MCLK_MULTIPLE_TX = 128;

/**
 * I2S MCLK Multiple for RX (ADC Input)
 *
 * Master clock divider for ADC input clock generation.
 * MCLK = Sample_Rate × MCLK_Multiple_RX
 * At 48 kHz: 48000 × 512 = 24.576 MHz MCLK
 *
 * Value: 512
 */
constexpr uint32_t I2S_MCLK_MULTIPLE_RX = 512;

/**
 * I2S Bit Clock Divisor
 *
 * Divider for generating bit clock (BCK) from MCLK.
 * Used in DMA clock configuration calculations.
 *
 * Value: 64
 */
constexpr uint32_t I2S_BCK_DIVISOR = 64;

// ==================================================================================
//                        FREERTOS TASK CONFIGURATION
// ==================================================================================

/**
 * Console Task Priority
 *
 * FreeRTOS priority level for the console task.
 * Higher values = higher priority (can preempt lower-priority tasks)
 *
 * Value: 2 (medium priority, preempts I/O but yields to audio)
 */
constexpr uint32_t LOG_TASK_PRIORITY = 2;

/**
 * Console Task Stack Size (Words)
 *
 * Memory allocated for console task stack.
 * Sufficient for string formatting and queue operations.
 *
 * Value: 4096 words (16 KB on 32-bit ESP32)
 */
constexpr uint32_t LOG_TASK_STACK_WORDS = 4096;

/**
 * Console Message Queue Length
 *
 * Maximum number of console log messages that can be queued before drops occur.
 * Drop-on-overflow policy ensures real-time tasks don't block on logging.
 *
 * Value: 128 messages
 */
constexpr std::size_t LOG_QUEUE_LENGTH = 128;

/**
 * VU Meter Task Priority
 *
 * FreeRTOS priority for display rendering task.
 * Lowest priority as visual updates are non-critical.
 *
 * Value: 1 (low priority, yields to all other tasks)
 */
constexpr uint32_t VU_TASK_PRIORITY = 1;

/**
 * VU Meter Task Stack Size (Words)
 *
 * Memory for VU meter task (display rendering).
 * Modest stack for graphics operations.
 *
 * Value: 4096 words (16 KB)
 */
constexpr uint32_t VU_TASK_STACK_WORDS = 4096;

/**
 * VU Meter Queue Length
 *
 * Mailbox-style queue: holds only the latest sample.
 * Old samples are overwritten - only current peak matters.
 *
 * Value: 1 (mailbox pattern)
 */
constexpr std::size_t VU_QUEUE_LENGTH = 1;

/**
 * RDS Assembler Task Priority
 *
 * FreeRTOS priority for RDS bitstream generation.
 * Low priority as RDS is asynchronous background task.
 *
 * Value: 1 (low priority)
 */
constexpr uint32_t RDS_TASK_PRIORITY = 1;

/**
 * RDS Assembler Task Stack Size (Words)
 *
 * Memory for RDS assembly and bitstream generation.
 *
 * Value: 4096 words (16 KB)
 */
constexpr uint32_t RDS_TASK_STACK_WORDS = 4096;

/**
 * RDS Bit Queue Length
 *
 * FIFO queue for RDS bitstream. Bits are strictly sequenced.
 * Pre-generated bits are queued for DSP synthesis.
 *
 * Value: 1024 bits
 */
constexpr std::size_t RDS_BIT_QUEUE_LENGTH = 1024;

/**
 * DSP Pipeline Task Priority
 *
 * Highest priority - audio processing cannot be interrupted.
 * Real-time constraints demand strict timing.
 *
 * Value: 6 (highest priority)
 */
constexpr uint32_t DSP_TASK_PRIORITY = 6;

/**
 * DSP Pipeline Task Stack Size (Words)
 *
 * Memory for audio processing buffers and DSP computations.
 * Includes FreeRTOS overhead for context switching.
 *
 * Value: 12288 words (48 KB)
 */
constexpr uint32_t DSP_TASK_STACK_WORDS = 12288;

// ==================================================================================
//                          VU METER DISPLAY LAYOUT
// ==================================================================================

/**
 * Display Width in Pixels
 *
 * ILI9341 screen width (landscape orientation).
 * Used for calculating bar positions and sizes.
 *
 * Value: 320 pixels
 */
constexpr int VU_DISPLAY_WIDTH = 320;

/**
 * Display Height in Pixels
 *
 * ILI9341 screen height (landscape orientation).
 *
 * Value: 240 pixels
 */
constexpr int VU_DISPLAY_HEIGHT = 240;

/**
 * VU Meter Horizontal Margin
 *
 * Padding on left and right edges of display.
 * Prevents bars from touching screen edges.
 *
 * Value: 16 pixels
 */
constexpr int VU_MARGIN_X = 16;

/**
 * VU Meter Y Coordinate (Top Position)
 *
 * Vertical position where VU bars begin.
 * Leaves space at top for title/status.
 *
 * Value: 32 pixels from top
 */
constexpr int VU_METER_Y = 32;

/**
 * VU Meter Label Width
 *
 * Space reserved for channel labels (L/R).
 * Remainder is available for bar graphics.
 *
 * Value: 14 pixels
 */
constexpr int VU_LABEL_WIDTH = 14;

/**
 * VU Meter Bar Height
 *
 * Height of each horizontal bar (left and right channels).
 *
 * Value: 22 pixels per bar
 */
constexpr int VU_BAR_HEIGHT = 22;

/**
 * VU Meter Bar Vertical Spacing
 *
 * Vertical distance between left and right channel bars.
 * Includes bar height plus gap between bars.
 *
 * Value: 32 pixels
 */
constexpr int VU_BAR_SPACING = 32;

/**
 * VU Meter Peak Indicator Width
 *
 * Width of the peak marker triangle at bar end.
 * Narrow indicator that shows instantaneous peak.
 *
 * Value: 3 pixels
 */
constexpr int VU_PEAK_WIDTH = 3;

// ==================================================================================
//                         VU METER COLOR DEFINITIONS (RGB565)
// ==================================================================================
//
// ILI9341 uses RGB565 format: 5 bits red, 6 bits green, 5 bits blue
// Formula: (R<<11) | (G<<5) | B, where R,G,B are 5, 6, 5 bit values
//

/** Black (off state) */
constexpr uint16_t COLOR_BLACK = 0x0000;

/** White (text and reference marks) */
constexpr uint16_t COLOR_WHITE = 0xFFFF;

/** Dark gray (background/grid) */
constexpr uint16_t COLOR_DARK_GRAY = 0x4208;

/** Green (nominal signal level -20 to -6 dBFS) */
constexpr uint16_t COLOR_GREEN = 0x07E0;

/** Yellow (yellow zone -6 to -3 dBFS, caution) */
constexpr uint16_t COLOR_YELLOW = 0xFFE0;

/** Orange (orange zone -3 to 0 dBFS, warning) */
constexpr uint16_t COLOR_ORANGE = 0xFD20;

/** Red (red zone 0 to +3 dBFS, clipping risk) */
constexpr uint16_t COLOR_RED = 0xF800;

// ==================================================================================
//                       VU METER COLOR ZONE THRESHOLDS
// ==================================================================================
//
// Bar fill zones as percentage of bar width:
//   0%  - Start of bar (minimum signal)
//   70% - Green zone ends, yellow zone begins
//   85% - Yellow zone ends, orange zone begins
//   95% - Orange zone ends, red zone begins
//   100% - Full bar (maximum signal/clipping)

/**
 * Green Zone Threshold
 *
 * Percentage of bar width where color transitions from green to yellow.
 * Below this: green (safe)
 * Above this: yellow (caution)
 *
 * Value: 0.70 (70% of bar width)
 */
constexpr float VU_GREEN_THRESHOLD = 0.70f;

/**
 * Yellow Zone Threshold
 *
 * Percentage of bar width where color transitions from yellow to orange.
 * Below this: yellow (caution)
 * Above this: orange (warning)
 *
 * Value: 0.85 (85% of bar width)
 */
constexpr float VU_YELLOW_THRESHOLD = 0.85f;

/**
 * Orange Zone Threshold
 *
 * Percentage of bar width where color transitions from orange to red.
 * Below this: orange (warning)
 * Above this: red (clipping risk)
 *
 * Value: 0.95 (95% of bar width)
 */
constexpr float VU_ORANGE_THRESHOLD = 0.95f;

// ==================================================================================
//                        VU METER BALLISTICS & DECAY
// ==================================================================================
//
// Peak meter ballistics define how quickly the needle responds to signal changes:
// - Attack: Speed of upward movement (fast response to peaks)
// - Release: Speed of downward movement when signal drops
// - Decay: Gradual fade of peak hold indicator
//

/**
 * VU Meter Attack Step
 *
 * How much the bar extends upward per update cycle.
 * Higher = faster response to sudden peaks.
 *
 * Value: 50 (aggressive peak tracking)
 */
constexpr uint16_t VU_ATTACK_STEP = 50;

/**
 * VU Meter Release Step
 *
 * How much the bar retracts per update cycle when signal drops.
 * Lower = smoother fallback, less jittery display.
 *
 * Value: 8 (smooth decay)
 */
constexpr uint16_t VU_RELEASE_STEP = 8;

/**
 * VU Meter Decay Interval (Milliseconds)
 *
 * How often release step is applied.
 * 16 ms = ~60 Hz update rate.
 *
 * Value: 16 milliseconds
 */
constexpr uint16_t VU_DECAY_INTERVAL_MS = 16;

/**
 * VU Meter Peak Hold Duration (Milliseconds)
 *
 * How long the peak indicator stays visible before fading.
 * Helps identify brief transient peaks that might otherwise be missed.
 *
 * Value: 1000 milliseconds (1 second)
 */
constexpr uint16_t VU_PEAK_HOLD_MS = 1000;

/**
 * VU Display Frame Refresh Interval (Milliseconds)
 *
 * Time between successive screen redraws.
 * 20 ms = 50 Hz refresh rate (matches typical LCD rates).
 *
 * Value: 20 milliseconds
 */
constexpr uint16_t VU_FRAME_INTERVAL_MS = 25;

// ==================================================================================
//                          VU METER dB SCALE RANGES
// ==================================================================================

/**
 * VU Meter Minimum dB Scale
 *
 * Lower limit of the dB scale displayed on the meter.
 * Signals below this are clipped to the bottom.
 *
 * Value: -40 dBFS (very quiet signals)
 */
constexpr float VU_DB_MIN = -40.0f;

/**
 * VU Meter Maximum dB Scale
 *
 * Upper limit of the dB scale displayed on the meter.
 * Signals above this indicate clipping risk.
 *
 * Value: 3 dBFS (just below full-scale, allowing headroom)
 */
constexpr float VU_DB_MAX = 3.0f;

/**
 * VU Scale Label Minimum
 *
 * Minimum label shown on the dB scale ruler.
 * (Internal scale for rendering reference marks)
 *
 * Value: -20 dBFS
 */
constexpr float VU_SCALE_LABEL_MIN = -20.0f;

/**
 * VU Scale Label Maximum
 *
 * Maximum label shown on the dB scale ruler.
 *
 * Value: 3 dBFS
 */
constexpr float VU_SCALE_LABEL_MAX = 3.0f;

// ==================================================================================
//                          RDS STRING LENGTHS
// ==================================================================================

/**
 * RDS Program Service (PS) Length
 *
 * Maximum length of station name displayed on radio.
 * FM standard: Exactly 8 ASCII characters.
 *
 * Value: 8 characters
 */
constexpr std::size_t RDS_PS_LENGTH = 8;

/**
 * RDS RadioText (RT) Maximum Length
 *
 * Maximum length of scrolling text message.
 * FM standard: Up to 64 ASCII characters (two 32-char versions A/B).
 *
 * Value: 64 characters
 */
constexpr std::size_t RDS_RT_MAX_LENGTH = 64;

/**
 * RDS LPF Baseband Filter Cutoff Frequency
 *
 * Low-pass filter applied to RDS baseband before synthesis.
 * Reduces aliasing and smooths waveform.
 *
 * Value: 2400 Hz
 */
constexpr float RDS_LPF_CUTOFF_HZ = 2400.0f;

// ==================================================================================
//                           DSP CLIPPING LIMITS
// ==================================================================================

/**
 * Soft Clipping Threshold
 *
 * Audio samples above this value are soft-clipped to prevent
 * harsh digital clipping artifacts.
 *
 * Value: 0.9999999f (just below full-scale)
 */
constexpr float SOFT_CLIP_LIMIT = 0.9999999f;

} // namespace Config

// =====================================================================================
//                                END OF FILE
// =====================================================================================
