/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                     Audio Engine Implementation
 *
 * =====================================================================================
 *
 * File:         DSP_pipeline.cpp
 * Description:  Implementation of the FM stereo encoding pipeline
 *
 * This file contains the complete implementation of the DSP_pipeline class, which
 * orchestrates the real-time DSP pipeline that converts 48 kHz stereo audio into
 * a 192 kHz FM multiplex (MPX) signal suitable for transmission.
 *
 * Key Features:
 *   • Real-time audio processing with strict timing constraints
 *   • Multi-stage DSP pipeline (8 stages)
 *   • Performance monitoring and statistics collection
 *   • VU meter data generation for display
 *   • Optional diagnostic logging for debugging
 *
 * Processing Flow:
 *   ADC (48 kHz) → Pre-emphasis → Notch Filter → Upsample (192 kHz) →
 *   Stereo Matrix → NCO Carriers → MPX Synthesis → RDS Injection → DAC (192 kHz)
 *
 * =====================================================================================
 */

#include "DSP_pipeline.h"
#include "Diagnostics.h"
#include "I2SDriver.h"
#include "Log.h"
#include "TaskStats.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp32-hal-cpu.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>

#include "RDSSynth.h"
#include "VUMeter.h"

// ==================================================================================
//                          I2S PORT CONFIGURATION
// ==================================================================================

namespace
{
    // I2S port assignments (ESP32-S3 has two independent I2S peripherals)
    constexpr i2s_port_t kI2SPortTx = I2S_NUM_1;  // DAC output: 192 kHz stereo
    constexpr i2s_port_t kI2SPortRx = I2S_NUM_0;  // ADC input: 48 kHz stereo
} // namespace

// ==================================================================================
//                          CONSTRUCTOR
// ==================================================================================

/**
 * DSP_pipeline Constructor
 *
 * Initializes the audio processing pipeline with default settings.
 * All buffers are zero-initialized by default (BSS section).
 *
 * Initialization:
 *   • pilot_19k_: NCO configured for 19 kHz pilot at 192 kHz sample rate
 *   • mpx_synth_: MPX mixer with pilot and stereo difference amplitudes
 *   • stats_: Performance statistics tracker (reset to zero)
 *
 * Note: Hardware I2S interfaces are NOT configured here. Call begin() to
 *       initialize I2S and load DSP filter coefficients.
 */
DSP_pipeline::DSP_pipeline()
    : pilot_19k_(19000.0f, static_cast<float>(Config::SAMPLE_RATE_DAC)),
      mpx_synth_(Config::PILOT_AMP, Config::DIFF_AMP)
{
    // Initialize performance statistics to zero
    stats_.reset();
}

// ==================================================================================
//                          INITIALIZATION (begin)
// ==================================================================================

/**
 * Initialize Audio Engine
 *
 * Sets up all hardware and software components required for real-time audio
 * processing. Called by the audio task after construction.
 *
 * Initialization Sequence:
 *   1. Verify SIMD support (ESP32-S3 specific optimization checks)
 *   2. Initialize I2S TX (192 kHz DAC output)
 *   3. Wait 100 ms for master clock (MCLK) to stabilize
 *   4. Initialize I2S RX (48 kHz ADC input)
 *   5. Configure pre-emphasis filter (50 µs time constant)
 *   6. Configure 19 kHz notch filter
 *   7. Initialize polyphase FIR upsampler (load coefficients)
 *   8. Configure RDS synthesizer (if enabled)
 *   9. Reset performance statistics
 *  10. Initialize task statistics monitoring
 *
 * Returns:
 *   true  - All initialization succeeded, ready to process audio
 *   false - Hardware initialization failed (check Serial for errors)
 *
 * Error Handling:
 *   If I2S setup fails, an error is logged and the function returns false.
 *   The audio task will exit if begin() fails.
 */
bool DSP_pipeline::begin()
{
    // Log system configuration
    Log::enqueue(Log::INFO, "ESP32-S3 Audio DSP: 48kHz -> 192kHz");

    // Verify SIMD/ESP32-S3 specific optimizations are available
    Diagnostics::verifySIMD();

    // Initialize I2S TX peripheral (DAC output at 192 kHz)
    // Must be initialized BEFORE RX to establish MCLK reference
    if (!AudioIO::setupTx())
    {
        Log::enqueue(Log::ERROR, "TX initialization failed!");
        return false;
    }

    // Wait for master clock to stabilize before starting RX
    // Critical for phase-locked loop (PLL) synchronization
    delay(100);

    // Initialize I2S RX peripheral (ADC input at 48 kHz)
    if (!AudioIO::setupRx())
    {
        Log::enqueue(Log::ERROR, "RX initialization failed!");
        return false;
    }

    // Configure pre-emphasis filter (50 µs for FM broadcast standard)
    // Alpha: filter coefficient, Gain: compensation for high-frequency boost
    preemphasis_.configure(Config::PREEMPHASIS_ALPHA, Config::PREEMPHASIS_GAIN);

    // Configure 19 kHz notch filter to prevent pilot tone interference
    // Removes any audio content at the pilot frequency
    notch_.configure(static_cast<float>(Config::SAMPLE_RATE_ADC),
                     Config::NOTCH_FREQUENCY_HZ, Config::NOTCH_RADIUS);

    // Initialize polyphase FIR upsampler (loads filter coefficients from flash)
    upsampler_.initialize();

    // Configure RDS synthesizer if RDS transmission is enabled
    // RDS assembler task must be started separately in setup()
    if (Config::RDS_ENABLE)
    {
        rds_synth_.configure(static_cast<float>(Config::SAMPLE_RATE_DAC));
    }

    // Reset performance statistics and start timing
    stats_.reset();
    uint64_t now         = esp_timer_get_time();
    stats_.start_time_us = now;  // Record system start time
    stats_.last_print_us = now;  // Initialize performance logging timer

    // Initialize FreeRTOS task statistics monitoring (if enabled in menuconfig)
    TaskStats::init();

    // Initialization complete, ready to process audio
    Log::enqueue(Log::INFO, "System Ready - Starting Audio Processing");

    return true;
}

// ==================================================================================
//                          MAIN AUDIO PROCESSING LOOP (process)
// ==================================================================================

/**
 * Process One Audio Block
 *
 * This is the heart of the FM stereo encoder. It processes exactly one block of
 * audio (64 samples @ 48 kHz = 1.33 ms) through the entire DSP pipeline.
 *
 * Called in an infinite loop by the audio task. Each call must complete within
 * 1.33 ms or audio glitches will occur.
 *
 * Processing Pipeline (8 stages):
 *   1. I2S Read:        Read 64 stereo samples from ADC (blocking)
 *   2. Int→Float:       Convert Q31 to normalized float, calculate VU meters
 *   3. Pre-emphasis:    Apply 50 µs FM pre-emphasis filter
 *   4. Notch Filter:    Remove 19 kHz content (pilot tone protection)
 *   5. Upsample:        48 kHz → 192 kHz using polyphase FIR filter with embedded 15 kHz low-pass
 *   6. Stereo Matrix:   Generate L+R (mono) and L-R (diff) signals
 *   7. MPX Synthesis:   Mix mono + pilot + (diff × 38 kHz) + RDS
 *   8. Float→Int:       Convert to Q31 and write to DAC
 *
 * Performance Monitoring:
 *   • Each stage is timed using CPU cycle counter
 *   • Statistics are updated for min/max/current processing time
 *   • VU meters are updated every 5 ms
 *   • Performance logs are printed every 5 seconds
 *
 * Real-Time Constraints:
 *   • Must complete in < 1333 µs (typical: ~300 µs)
 *   • Target CPU usage: < 30% (leaves 70% headroom)
 *   • No dynamic memory allocation in process()
 *   • All buffers are pre-allocated
 */
void DSP_pipeline::process()
{
    using namespace Config;

    // ============================================================================
    // I2S READ: Acquire Audio Data from ADC
    // ============================================================================
    //
    // Read one block of stereo audio from the I2S RX peripheral (ADC).
    // This call BLOCKS until the I2S DMA buffer has a complete block ready.
    // Typical latency: ~1.33 ms (one block period)
    //
    // Buffer format: int32_t interleaved stereo [L, R, L, R, ...]
    // Sample format: Q31 fixed-point (24-bit audio in 32-bit container)
    //
    size_t    bytes_read = 0;
    esp_err_t ret =
        i2s_read(kI2SPortRx, rx_buffer_, sizeof(rx_buffer_), &bytes_read, portMAX_DELAY);

    // Check for I2S read errors
    if (ret != ESP_OK)
    {
        Log::enqueuef(Log::ERROR, "Read error: %d", ret);
        ++stats_.errors;
        return; // Skip this cycle to maintain pipeline timing
    }

    // Check if any data was received
    if (bytes_read == 0)
    {
        return; // No data available, wait for next DMA interrupt
    }

    // Calculate number of stereo frames received
    // Each frame = 2 samples (L+R) × 4 bytes (32-bit container) = 8 bytes
    std::size_t frames_read = bytes_read / (2 * static_cast<std::size_t>(BYTES_PER_SAMPLE));
    if (frames_read == 0)
    {
        return; // Incomplete frame, skip processing
    }

    // ============================================================================
    // Performance Timing Setup
    // ============================================================================
    //
    // Capture CPU cycle count for precise timing measurements.
    // ESP32-S3 cycle counter runs at CPU frequency (typically 240 MHz).
    //
    uint32_t cpu_mhz = getCpuFrequencyMhz();  // Typically 240 MHz
    uint32_t t_start = ESP.getCycleCount();   // Start of processing

    // ============================================================================
    // STAGE 1: Integer to Float Conversion + VU Meter Calculation
    // ============================================================================
    //
    // Convert fixed-point Q31 samples (int32) to floating-point [-1.0, +1.0].
    // Simultaneously calculate peak and RMS levels for VU meter display.
    //
    // Q31 Format:
    //   • Full scale = ±2,147,483,647 (±2^31 - 1)
    //   • Normalized range: [-1.0, +1.0]
    //   • Conversion: float = int32 / 2147483647.0
    //
    // VU Meter Metrics:
    //   • Peak: Maximum absolute sample value per channel
    //   • RMS: Root mean square (power average) per channel
    //
    uint32_t t0       = t_start;  // Start timing for Stage 1
    float    l_sum_sq = 0.0f;     // Left channel squared sum (for RMS)
    float    r_sum_sq = 0.0f;     // Right channel squared sum (for RMS)
    float    l_peak   = 0.0f;     // Left channel peak level
    float    r_peak   = 0.0f;     // Right channel peak level

    // Process all stereo frames in the block
    for (std::size_t f = 0; f < frames_read; ++f)
    {
        // Calculate buffer indices for interleaved stereo
        // Format: [L0, R0, L1, R1, L2, R2, ...]
        std::size_t iL = (f * 2) + 0;  // Left channel (even indices)
        std::size_t iR = (f * 2) + 1;  // Right channel (odd indices)

        // Convert Q31 fixed-point to normalized float [-1.0, +1.0]
        float vl = static_cast<float>(rx_buffer_[iL]) / Q31_FULL_SCALE;
        float vr = static_cast<float>(rx_buffer_[iR]) / Q31_FULL_SCALE;

        // Store converted samples in floating-point buffer
        rx_f32_[iL] = vl;
        rx_f32_[iR] = vr;

        // Accumulate squared samples for RMS calculation
        l_sum_sq += vl * vl;
        r_sum_sq += vr * vr;

        // Track peak levels (absolute maximum)
        l_peak = std::max(l_peak, std::fabs(vl));
        r_peak = std::max(r_peak, std::fabs(vr));
    }

    // Calculate RMS (root mean square) levels for each channel
    // RMS = sqrt(sum of squares / number of samples)
    float l_rms     = (frames_read > 0) ? sqrtf(l_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    float r_rms     = (frames_read > 0) ? sqrtf(r_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    float input_rms = (l_rms + r_rms) * 0.5f;  // Average RMS for overall level

    // Measure Stage 1 processing time
    uint32_t t1        = ESP.getCycleCount();
    uint32_t stage1_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_int_to_float.update(stage1_us);

    // ============================================================================
    // VU Meter Update (Throttled)
    // ============================================================================
    //
    // Send audio level data to the VU meter task for display.
    // Updates are throttled to reduce cross-core queue traffic.
    //
    // Update interval: Every 5 ms (200 Hz refresh rate)
    // Queue: FreeRTOS queue from Core 0 (audio) → Core 1 (display)
    //
    {
        static uint64_t s_last_vu_us = 0;  // Last VU update timestamp (static preserves between calls)
        uint64_t        now_us       = esp_timer_get_time();

        // Check if enough time has elapsed since last VU update
        if ((now_us - s_last_vu_us) >= Config::VU_UPDATE_INTERVAL_US)
        {
            s_last_vu_us = now_us;  // Update timestamp for next throttle check

            // Prepare VU meter sample data
            VUMeter::Sample vu{};
            vu.l_rms  = l_rms;   // Left channel RMS level
            vu.r_rms  = r_rms;   // Right channel RMS level
            vu.l_peak = l_peak;  // Left channel peak level
            vu.r_peak = r_peak;  // Right channel peak level

            // Convert peak levels to dBFS (decibels relative to full scale)
            // Formula: dBFS = 20 * log10(level / reference)
            // Silence (0.0) maps to -120 dBFS (effectively -∞)
            vu.l_dbfs = (l_rms > 0.0f) ? (20.0f * log10f(std::min(l_peak, Config::DBFS_REF) /
                                                         Config::DBFS_REF))
                                       : -120.0f;
            vu.r_dbfs = (r_rms > 0.0f) ? (20.0f * log10f(std::min(r_peak, Config::DBFS_REF) /
                                                         Config::DBFS_REF))
                                       : -120.0f;

            vu.frames = static_cast<uint32_t>(frames_read);            // Number of frames in this block
            vu.ts_us  = static_cast<uint32_t>(now_us & 0xFFFFFFFFu);  // Timestamp (32-bit truncated)

            // Send VU data to display task (non-blocking queue write)
            VUMeter::enqueue(vu);
        }
    }

    // ============================================================================
    // STAGE 2: FM Pre-Emphasis Filter
    // ============================================================================
    //
    // Apply 50 µs FM pre-emphasis to boost high frequencies.
    // This is required for FM broadcast to improve SNR at the receiver.
    //
    // Filter Type: First-order IIR high-pass filter
    // Time Constant: 50 µs (FM broadcast standard in Europe/Americas)
    // Frequency Response: +6 dB/octave above ~3.18 kHz
    //
    // Purpose:
    //   Pre-emphasis compensates for FM's inherent high-frequency noise.
    //   The receiver applies de-emphasis (inverse filter) to restore flat response.
    //
    t0 = t1;  // Start timing Stage 2
    preemphasis_.process(rx_f32_, frames_read);
    t1                 = ESP.getCycleCount();
    uint32_t stage2_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_preemphasis.update(stage2_us);

    // ============================================================================
    // STAGE 3: 19 kHz Notch Filter
    // ============================================================================
    //
    // Remove any audio content at 19 kHz to prevent interference with the pilot tone.
    // The 19 kHz pilot is used by FM stereo receivers to synchronize demodulation.
    //
    // Filter Type: Second-order IIR notch (band-reject) filter
    // Center Frequency: 19 kHz
    // Q Factor: Controlled by radius parameter (narrow notch)
    //
    // Purpose:
    //   Audio signals at 19 kHz would interfere with the pilot tone, causing
    //   distortion in the stereo decoding process. This filter ensures a clean
    //   19 kHz region for the pilot.
    //
    t0 = t1;  // Start timing Stage 3
    notch_.process(rx_f32_, frames_read);
    t1                      = ESP.getCycleCount();
    uint32_t stage_notch_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_notch.update(stage_notch_us);

    // ============================================================================
    // STAGE 4: Polyphase FIR Upsampler (48 kHz → 192 kHz)
    // ============================================================================
    //
    // Upsample audio from 48 kHz to 192 kHz using a 4× polyphase FIR filter.
    // The higher sample rate is required for FM multiplex synthesis.
    //
    // Upsampling Factor: 4× (48 kHz × 4 = 192 kHz)
    // Filter Type: 96-tap polyphase FIR (Kaiser-windowed sinc)
    // Passband: 0–15 kHz (FM audio limit)
    // Stopband: ≥19 kHz (pilot protection, removes imaging artifacts)
    //
    // Input:  64 stereo frames @ 48 kHz  (128 samples total)
    // Output: 256 stereo frames @ 192 kHz (512 samples total)
    //
    // Purpose:
    //   FM stereo requires mixing audio with 19 kHz pilot and 38 kHz subcarrier.
    //   To avoid aliasing, we need at least 4× the highest frequency component.
    //   192 kHz provides enough bandwidth for audio (0-15 kHz), pilot (19 kHz),
    //   stereo subcarrier (23-53 kHz), and RDS (57 kHz).
    //
    t0 = t1;  // Start timing Stage 4
    upsampler_.process(rx_f32_, tx_f32_, frames_read);
    t1                 = ESP.getCycleCount();
    uint32_t stage3_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_upsample.update(stage3_us);

    // ============================================================================
    // STAGE 5: Stereo Matrix (L+R and L-R Generation)
    // ============================================================================
    //
    // Convert left/right stereo channels into mono (L+R) and difference (L-R) signals.
    // This is the FM stereo encoding matrix.
    //
    // Input:  L and R channels (interleaved stereo @ 192 kHz)
    // Output: Mono = L+R  (main channel, carries full compatibility)
    //         Diff = L-R  (stereo difference, modulated onto 38 kHz subcarrier)
    //
    // FM Stereo Compatibility:
    //   • Mono receivers: Decode only L+R (full audio, no stereo)
    //   • Stereo receivers: Decode (L+R) and (L-R), then recover:
    //       L = (L+R + L-R) / 2 = (2L) / 2 = L
    //       R = (L+R - L-R) / 2 = (2R) / 2 = R
    //
    t0                  = t1;  // Start timing Stage 5
    std::size_t samples = frames_read * UPSAMPLE_FACTOR;  // 256 samples per channel @ 192 kHz
    stereo_matrix_.process(tx_f32_, mono_buffer_, diff_buffer_, samples);
    t1                       = ESP.getCycleCount();
    uint32_t stage_matrix_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_matrix.update(stage_matrix_us);

    // ============================================================================
    // STAGE 6: FM Multiplex (MPX) Synthesis
    // ============================================================================
    //
    // Combine mono, pilot, and stereo subcarrier into the final FM multiplex signal.
    // This is the composite baseband signal that modulates the FM carrier.
    //
    // FM Stereo Multiplex Spectrum:
    //   0-15 kHz:   L+R (mono audio, main channel)
    //   19 kHz:     Pilot tone (9% modulation, synchronization reference)
    //   23-53 kHz:  (L-R) × 38 kHz DSB-SC (stereo difference, suppressed carrier)
    //   57 kHz:     RDS subcarrier (optional, 57 kHz = 3 × 19 kHz)
    //
    // Phase Coherence:
    //   All carriers (19 kHz, 38 kHz, 57 kHz) are derived from a single NCO
    //   to maintain perfect phase coherence. This is critical for stereo decoding.
    //
    // Formula:
    //   MPX = (L+R) + pilot × 0.09 + (L-R) × cos(2π × 38 kHz × t) × 0.90 + RDS
    //
    t0 = t1;  // Start timing Stage 6

    // Sub-stage 6a: Generate phase-coherent carriers from master 19 kHz NCO
    // Pilot = 19 kHz, Subcarrier = 38 kHz (2nd harmonic), RDS = 57 kHz (3rd harmonic)
    pilot_19k_.generate_harmonics(pilot_buffer_, subcarrier_buffer_, carrier57_buffer_, samples);

    // Sub-stage 6b: Mix mono, pilot, and modulated stereo difference
    // MPX = mono + (pilot × pilot_amp) + (diff × subcarrier × diff_amp)
    mpx_synth_.process(mono_buffer_, diff_buffer_, pilot_buffer_, subcarrier_buffer_, mpx_buffer_,
                       samples);

    // Sub-stage 6c: RDS Injection (Optional)
    // Adds RDS (Radio Data System) subcarrier at 57 kHz
    // RDS carries station info, song metadata, traffic alerts, etc.
    if (Config::RDS_ENABLE)
    {
        uint32_t t_r0 = ESP.getCycleCount();  // Time RDS processing separately

        // Modulate RDS bitstream onto 57 kHz carrier
        // carrier57_buffer_ is phase-locked to pilot (57 kHz = 3 × 19 kHz)
        rds_synth_.processBlockWithCarrier(carrier57_buffer_, Config::RDS_AMP, rds_buffer_,
                                           samples);

        // Mix RDS subcarrier into MPX signal
        for (std::size_t i = 0; i < samples; ++i)
        {
            mpx_buffer_[i] += rds_buffer_[i];
        }

        uint32_t t_r1   = ESP.getCycleCount();
        uint32_t rds_us = (t_r1 - t_r0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
        stats_.stage_rds.update(rds_us);
    }

    // Sub-stage 6d: Duplicate MPX to stereo DAC channels
    // The MPX signal is mono (composite baseband), but I2S requires stereo output.
    // Both L and R DAC channels carry the same MPX signal.
    for (std::size_t i = 0; i < samples; ++i)
    {
        float mpx          = mpx_buffer_[i];  // Get MPX sample
        tx_f32_[i * 2 + 0] = mpx;             // Left DAC channel
        tx_f32_[i * 2 + 1] = mpx;             // Right DAC channel (duplicate)
    }

    t1                    = ESP.getCycleCount();
    uint32_t stage_mpx_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_mpx.update(stage_mpx_us);

    // ============================================================================
    // STAGE 7: Float to Integer Conversion + Output Level Monitoring
    // ============================================================================
    //
    // Convert floating-point MPX signal back to Q31 fixed-point for DAC output.
    // Apply soft clipping to prevent hard clipping distortion at the DAC.
    //
    // Conversion:
    //   float [-1.0, +1.0] → int32 Q31 [−2^31, +2^31−1]
    //   Multiply by 2,147,483,647 (INT32_MAX)
    //
    // Clipping Strategy:
    //   • Clamp to [-1.0, +0.9999999] to leave DAC headroom
    //   • This is the ONLY clipping point in the entire pipeline
    //   • All other stages maintain linear operation
    //
    // Purpose:
    //   Final stage before DAC transmission. Ensures signal stays within
    //   hardware limits while maintaining maximum dynamic range.
    //
    t0                        = t1;  // Start timing Stage 7
    float       sum_output_sq = 0.0f;  // For output RMS calculation
    std::size_t out_samples   = frames_read * UPSAMPLE_FACTOR * 2;  // 256 frames × 2 channels = 512 samples

    for (std::size_t i = 0; i < out_samples; ++i)
    {
        float v = tx_f32_[i];  // Get floating-point sample

        // Soft clip to prevent DAC overload
        // Max: +0.9999999 (just below full scale)
        // Min: -1.0 (symmetric clipping)
        v = std::min(0.9999999f, std::max(-1.0f, v));

        // Accumulate for output RMS monitoring
        sum_output_sq += v * v;

        // Convert to Q31 fixed-point (int32)
        // Multiply by INT32_MAX (2,147,483,647)
        tx_buffer_[i] = static_cast<int32_t>(v * 2147483647.0f);
    }

    // Calculate output RMS level (for monitoring/diagnostics)
    float output_rms = 0.0f;
    if (out_samples > 0)
    {
        output_rms = sqrtf(sum_output_sq / static_cast<float>(out_samples));
    }

    // Measure Stage 7 processing time
    t1                 = ESP.getCycleCount();
    uint32_t stage4_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_float_to_int.update(stage4_us);

    // Note: Overall pipeline gain is no longer logged (removed per design decision)

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    int32_t peak_adc = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);
#endif

    // ============================================================================
    // Performance Monitoring: Calculate Total Processing Time
    // ============================================================================
    //
    // Measure end-to-end processing latency from I2S read to I2S write.
    // This includes all 7 DSP stages but excludes I2S DMA wait times.
    //
    uint32_t t_end    = t1;  // End of all DSP processing
    uint32_t total_us = (t_end - t_start) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.total.update(total_us);  // Update min/max/current statistics

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    // ============================================================================
    // Diagnostic Signal Level Monitoring (Optional)
    // ============================================================================
    //
    // Track signal levels through the pipeline for debugging.
    // Only compiled if DIAGNOSTIC_PRINT_INTERVAL > 0 in Config.h.
    //
    int32_t peak_adc = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);

    // Convert pre-emphasis output back to int32 to measure peak level
    int32_t peak_after_pre = 0;
    for (std::size_t i = 0; i < frames_read * 2; ++i)
    {
        float v       = rx_f32_[i];
        v             = std::min(0.9999999f, std::max(-1.0f, v));
        rx_buffer_[i] = static_cast<int32_t>(v * 2147483647.0f);
    }
    peak_after_pre = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);

    // Measure peak level after FIR upsampler
    int32_t peak_after_fir =
        Diagnostics::findPeakAbs(tx_buffer_, frames_read * UPSAMPLE_FACTOR * 2);

    // Print diagnostics every N blocks (throttled)
    ++diagnostic_counter_;
    if (diagnostic_counter_ >= DIAGNOSTIC_PRINT_INTERVAL)
    {
        diagnostic_counter_ = 0;

        // Calculate gain in dB at each stage
        float pre_db =
            20.0f * log10f(static_cast<float>(peak_after_pre) / static_cast<float>(peak_adc));
        float total_db =
            20.0f * log10f(static_cast<float>(peak_after_fir) / static_cast<float>(peak_adc));

        printDiagnostics(frames_read, peak_adc, peak_after_pre, peak_after_fir, pre_db, total_db);
    }
#endif

    // ============================================================================
    // STAGE 8: I2S Write (DAC Output)
    // ============================================================================
    //
    // Write processed MPX signal to DAC via I2S TX peripheral.
    // This call BLOCKS until the I2S DMA buffer has space available.
    //
    // Output format: 192 kHz stereo, Q31 fixed-point
    // Buffer size: 256 stereo frames × 2 channels × 4 bytes = 2048 bytes
    //
    size_t bytes_to_write = frames_read * UPSAMPLE_FACTOR * 2 * BYTES_PER_SAMPLE;
    size_t bytes_written  = 0;

    // Write to I2S TX (blocking call)
    ret = i2s_write(kI2SPortTx, tx_buffer_, bytes_to_write, &bytes_written, portMAX_DELAY);

    // Check for I2S write errors
    if (ret != ESP_OK)
    {
        Log::enqueuef(Log::ERROR, "Write error: %d", ret);
        ++stats_.errors;
    }

    // Check for underrun (incomplete write)
    // Underruns indicate the DAC consumed data faster than we could produce it
    if (bytes_written != bytes_to_write)
    {
        Log::enqueuef(Log::WARN, "Underrun (wrote %u/%u bytes)", (unsigned)bytes_written,
                      (unsigned)bytes_to_write);
    }

    // Increment loop counter (for statistics and uptime tracking)
    ++stats_.loops_completed;

    // ============================================================================
    // Performance Logging (Throttled to Every 5 Seconds)
    // ============================================================================
    //
    // Print detailed performance statistics to Serial console via logger queue.
    // Includes CPU usage, headroom, per-stage timing, and memory info.
    //
    uint64_t now = esp_timer_get_time();
    if (now - stats_.last_print_us >= STATS_PRINT_INTERVAL_US)
    {
        stats_.last_print_us = now;  // Update timestamp for next log

        // Calculate available time per block (based on sample rate)
        // 64 samples @ 48 kHz = 64 / 48000 = 1.333 ms = 1333 µs
        float available_us = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);

        // Calculate CPU usage percentage
        // Usage = (processing time / available time) × 100%
        float cpu_usage =
            (available_us > 0.0f) ? (stats_.total.current / available_us * 100.0f) : 0.0f;

        // Calculate headroom (unused processing time)
        // Headroom = 100% - usage (higher is better, indicates jitter tolerance)
        float cpu_headroom = 100.0f - cpu_usage;

        // Print formatted performance report to Serial
        printPerformance(frames_read, available_us, cpu_usage, cpu_headroom);
    }

    // ============================================================================
    // Status Panel Update (Throttled to Every 100 ms)
    // ============================================================================
    //
    // Send compact statistics snapshot to VU meter task for on-screen display.
    // Update rate is higher than performance logging to keep display responsive.
    //
    {
        static uint64_t s_last_status_us = 0;  // Last status update timestamp
        if ((now - s_last_status_us) >= Config::STATUS_PANEL_UPDATE_US)
        {
            s_last_status_us = now;  // Update timestamp for next status update

            // Recalculate CPU usage/headroom (independent of 5-second logging gate)
            float available_us = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);
            float cpu_usage =
                (available_us > 0.0f) ? (stats_.total.current / available_us * 100.0f) : 0.0f;
            float cpu_headroom = 100.0f - cpu_usage;

            // Populate status snapshot structure
            VUMeter::StatsSnapshot snap{};

            // CPU and timing statistics
            snap.cpu_usage     = cpu_usage;       // Current CPU usage percentage
            snap.cpu_headroom  = cpu_headroom;    // Current headroom percentage
            snap.total_us_cur  = static_cast<float>(stats_.total.current);  // Current total time
            snap.total_us_min  = static_cast<float>(stats_.total.min);      // Minimum total time
            snap.total_us_max  = static_cast<float>(stats_.total.max);      // Maximum total time

            // Per-stage timing (for detailed performance view)
            snap.fir_us_cur    = static_cast<float>(stats_.stage_upsample.current);  // FIR upsampler
            snap.mpx_us_cur    = static_cast<float>(stats_.stage_mpx.current);       // MPX synthesis
            snap.matrix_us_cur = static_cast<float>(stats_.stage_matrix.current);    // Stereo matrix
            snap.rds_us_cur    = static_cast<float>(stats_.stage_rds.current);       // RDS injection

            // Memory statistics
            snap.heap_free = ESP.getFreeHeap();     // Current free heap (bytes)
            snap.heap_min  = ESP.getMinFreeHeap();  // Minimum free heap ever seen (bytes)

            // System statistics
            snap.uptime_s =
                static_cast<uint32_t>((esp_timer_get_time() - stats_.start_time_us) / 1000000ULL);
            snap.loops_completed = stats_.loops_completed;  // Total blocks processed
            snap.errors          = stats_.errors;           // Total I2S errors

            // Optional: FreeRTOS task statistics (per-core CPU load, stack usage)
            // Only available if CONFIG_FREERTOS_USE_TRACE_FACILITY is enabled
            float    core0 = 0, core1 = 0, aud = 0, logg = 0, vu = 0;
            uint32_t a_sw = 0, l_sw = 0, v_sw = 0;
            bool     cpu_ok = TaskStats::collect(core0, core1, aud, logg, vu, a_sw, l_sw, v_sw);

            if (cpu_ok)
            {
                // Task statistics are available
                snap.core0_load              = core0;  // Core 0 load (audio core)
                snap.core1_load              = core1;  // Core 1 load (display/logger core)
                snap.audio_cpu               = aud;    // Audio task CPU percentage
                snap.logger_cpu              = logg;   // Logger task CPU percentage
                snap.vu_cpu                  = vu;     // VU meter task CPU percentage
                snap.audio_stack_free_words  = a_sw;   // Audio task free stack (words)
                snap.logger_stack_free_words = l_sw;   // Logger task free stack (words)
                snap.vu_stack_free_words     = v_sw;   // VU task free stack (words)
                snap.cpu_valid               = 1;      // Flag: task stats valid
            }
            else
            {
                // Task statistics not available (FreeRTOS trace not enabled)
                snap.cpu_valid = 0;
            }

            // Send status snapshot to VU meter task (non-blocking)
            VUMeter::enqueueStats(snap);
        }
    }
}

// ==================================================================================
//                          FREERTOS TASK MANAGEMENT
// ==================================================================================

/**
 * Start Audio Task (Instance Method)
 *
 * Spawns a FreeRTOS task to run the audio processing loop for this specific
 * DSP_pipeline instance. Called internally by the static startTask() facade.
 *
 * Parameters:
 *   core_id:      CPU core to pin the task (0 = audio core, 1 = display core)
 *   priority:     FreeRTOS task priority (higher = more urgent, typically 6)
 *   stack_words:  Stack size in 32-bit words (not bytes!)
 *
 * Task Configuration:
 *   • Name: "audio"
 *   • Core: 0 (dedicated audio core, no Wi-Fi interrupts)
 *   • Priority: 6 (highest priority for real-time audio)
 *   • Stack: 12288 words = 49152 bytes = 48 KB
 *
 * Returns:
 *   true:  Task created successfully
 *   false: Task creation failed (out of memory or invalid parameters)
 */
bool DSP_pipeline::startTaskInstance(int core_id, UBaseType_t priority, uint32_t stack_words)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        taskTrampoline,   // Task function (static entry point)
        "audio",          // Task name (for debugging)
        stack_words,      // Stack size in words (not bytes!)
        this,             // Parameter passed to task (DSP_pipeline* instance)
        priority,         // Task priority (6 = highest)
        &task_handle_,    // Task handle (for suspend/resume/delete)
        core_id           // CPU core (0 = audio core)
    );
    return ok == pdPASS;
}

/**
 * Task Trampoline (Static Entry Point)
 *
 * FreeRTOS task entry point. Receives DSP_pipeline* as parameter, calls begin()
 * to initialize hardware, then enters infinite process() loop.
 *
 * This function never returns under normal operation. If begin() fails, the
 * task exits gracefully by calling vTaskDelete(nullptr).
 *
 * Parameters:
 *   arg:  Pointer to DSP_pipeline instance (void* for FreeRTOS compatibility)
 *
 * Execution Flow:
 *   1. Cast arg to DSP_pipeline*
 *   2. Call begin() to initialize I2S and DSP modules
 *   3. If begin() fails, log error and exit task
 *   4. Enter infinite loop calling process() (never returns)
 */
void DSP_pipeline::taskTrampoline(void* arg)
{
    // Cast void* parameter to DSP_pipeline* instance
    auto* self = static_cast<DSP_pipeline*>(arg);

    // Initialize hardware and DSP modules
    if (!self->begin())
    {
        Log::enqueue(Log::ERROR, "DSP_pipeline begin() failed");
        vTaskDelete(nullptr);  // Exit task on initialization failure
        return;
    }

    // Infinite audio processing loop (never returns)
    for (;;)
    {
        self->process();  // Process one audio block (~1.33 ms @ 48 kHz)
    }
}

/**
 * Start Audio Task (Static Singleton Facade)
 *
 * Convenience method that creates a managed DSP_pipeline singleton and spawns
 * its task. This is the recommended way to start the audio engine from main code.
 *
 * Singleton Pattern:
 *   A static DSP_pipeline instance is created on first call and reused thereafter.
 *   Only one audio engine can exist per application.
 *
 * Parameters:
 *   core_id:      CPU core (must be 0 for audio)
 *   priority:     Task priority (typically 6, highest priority)
 *   stack_words:  Stack size in words (typically 12288 = 48 KB)
 *
 * Returns:
 *   true:  Audio task started successfully
 *   false: Task creation failed
 *
 * Example Usage:
 *   void setup() {
 *       DSP_pipeline::startTask(0, 6, 12288);  // Start audio on Core 0
 *   }
 */
bool DSP_pipeline::startTask(int core_id, UBaseType_t priority, uint32_t stack_words)
{
    static DSP_pipeline s_instance;  // Singleton instance (created once)
    return s_instance.startTaskInstance(core_id, priority, stack_words);
}

// ==================================================================================
//                          PERFORMANCE LOGGING
// ==================================================================================

/**
 * Print Performance Statistics
 *
 * Formats and logs detailed performance metrics to the Serial console via the
 * logger queue. Called every 5 seconds by process() to monitor system health.
 *
 * Parameters:
 *   frames_read:   Number of frames processed in this block (typically 64)
 *   available_us:  Available processing time per block (1333 µs @ 48 kHz)
 *   cpu_usage:     CPU usage percentage (0-100%)
 *   cpu_headroom:  CPU headroom percentage (0-100%, higher is better)
 *
 * Output Format:
 *   ========================================
 *   Performance Stats
 *   ========================================
 *   Loops completed: 3600
 *   Errors: 0
 *   Uptime: 60.0 seconds
 *   ----------------------------------------
 *   Processing time:
 *     Current: 298.45 µs
 *     Min: 285.12 µs
 *     Max: 312.78 µs
 *     Available: 1333.33 µs
 *   CPU usage: 22.4%
 *   CPU headroom: 77.6%
 *   ----------------------------------------
 *   Per-Stage Breakdown:
 *     1. Deinterleave (int→float): 12.34 µs
 *     2. Pre-emphasis: 18.56 µs
 *     ... (all 8 stages)
 *   ----------------------------------------
 *   Free heap: 245678 bytes
 *   Min free heap: 234567 bytes
 *   ========================================
 */
void DSP_pipeline::printPerformance(std::size_t frames_read, float available_us, float cpu_usage,
                                   float cpu_headroom)
{
    using namespace Config;

    (void)frames_read;  // Unused parameter (reserved for future use)

    // Print formatted header
    Log::enqueue(Log::INFO, "========================================");
    Log::enqueue(Log::INFO, "Performance Stats");
    Log::enqueue(Log::INFO, "========================================");

    // Summary
    Log::enqueuef(Log::INFO, "Loops completed: %u", stats_.loops_completed);
    Log::enqueuef(Log::INFO, "Errors: %u", stats_.errors);
    float uptime_s = (esp_timer_get_time() - stats_.start_time_us) / 1'000'000.0f;
    Log::enqueuef(Log::INFO, "Uptime: %.1f seconds", uptime_s);

    // Processing times
    Log::enqueue(Log::INFO, "----------------------------------------");
    Log::enqueue(Log::INFO, "Processing time:");
    Log::enqueuef(Log::INFO, "  Current: %.2f µs", (double)stats_.total.current);
    Log::enqueuef(Log::INFO, "  Min: %.2f µs", (double)stats_.total.min);
    Log::enqueuef(Log::INFO, "  Max: %.2f µs", (double)stats_.total.max);
    Log::enqueuef(Log::INFO, "  Available: %.2f µs", (double)available_us);
    Log::enqueuef(Log::INFO, "CPU usage: %.1f%%", (double)cpu_usage);
    Log::enqueuef(Log::INFO, "CPU headroom: %.1f%%", (double)cpu_headroom);

    // Per-stage breakdown
    Log::enqueue(Log::INFO, "----------------------------------------");
    Log::enqueue(Log::INFO, "Per-Stage Breakdown:");
    Log::enqueue(Log::INFO, "  1. Deinterleave (int→float):");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_int_to_float.current, (double)stats_.stage_int_to_float.min,
                  (double)stats_.stage_int_to_float.max);
    Log::enqueue(Log::INFO, "  2. Gain processing:");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_preemphasis.current, (double)stats_.stage_preemphasis.min,
                  (double)stats_.stage_preemphasis.max);
    Log::enqueue(Log::INFO, "  3. 19 kHz notch:");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_notch.current, (double)stats_.stage_notch.min,
                  (double)stats_.stage_notch.max);
    Log::enqueue(Log::INFO, "  4. Upsample 4× (FIR):");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_upsample.current, (double)stats_.stage_upsample.min,
                  (double)stats_.stage_upsample.max);
    Log::enqueue(Log::INFO, "  5. Stereo matrix:");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_matrix.current, (double)stats_.stage_matrix.min,
                  (double)stats_.stage_matrix.max);
    Log::enqueue(Log::INFO, "  6. MPX synthesis:");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_mpx.current, (double)stats_.stage_mpx.min,
                  (double)stats_.stage_mpx.max);
    Log::enqueue(Log::INFO, "  7. RDS injection:");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_rds.current, (double)stats_.stage_rds.min,
                  (double)stats_.stage_rds.max);
    Log::enqueue(Log::INFO, "  8. Conversion (float→int):");
    Log::enqueuef(Log::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_float_to_int.current, (double)stats_.stage_float_to_int.min,
                  (double)stats_.stage_float_to_int.max);

    // Footer
    Log::enqueue(Log::INFO, "----------------------------------------");
    Log::enqueuef(Log::INFO, "Free heap: %u bytes", (unsigned)ESP.getFreeHeap());
    Log::enqueuef(Log::INFO, "Min free heap: %u bytes", (unsigned)ESP.getMinFreeHeap());
    // Gain reporting removed per request
    Log::enqueue(Log::INFO, "========================================");
}

// ==================================================================================
//                          DIAGNOSTIC LOGGING (OPTIONAL)
// ==================================================================================

/**
 * Print Diagnostic Signal Levels
 *
 * Logs signal levels at various points in the DSP pipeline for debugging.
 * Only compiled and called if DIAGNOSTIC_PRINT_INTERVAL > 0 in Config.h.
 *
 * This function helps diagnose gain staging issues by showing how signal levels
 * change through the pipeline. Useful for tuning pre-emphasis gain and preventing
 * clipping.
 *
 * Parameters:
 *   frames_read:  Number of frames processed (unused, reserved for future use)
 *   peak_adc:     Peak ADC input level (Q31 fixed-point)
 *   peak_pre:     Peak level after pre-emphasis (Q31 fixed-point)
 *   peak_fir:     Peak level after FIR upsampler (Q31 fixed-point)
 *   pre_db:       Pre-emphasis gain in dB (peak_pre / peak_adc)
 *   total_db:     Total pipeline gain in dB (peak_fir / peak_adc)
 *
 * Output Example:
 *   === SIGNAL LEVEL DIAGNOSTIC ===
 *   ADC Peak: 536870912 (25.0%)
 *   After Pre: 644245094 (30.0%)  Pre Gain: 1.56 dB
 *   After FIR: 751619277 (35.0%)  Total Gain: 2.88 dB
 *
 * Interpretation:
 *   • ADC Peak shows input level (should be 50-80% for optimal SNR)
 *   • Pre Gain shows high-frequency boost from pre-emphasis
 *   • Total Gain shows overall level increase through entire pipeline
 *   • If Total Gain is too high (>6 dB), reduce PREEMPHASIS_GAIN in config
 */
void DSP_pipeline::printDiagnostics(std::size_t frames_read, int32_t peak_adc, int32_t peak_pre,
                                   int32_t peak_fir, float pre_db, float total_db)
{
    using namespace Config;

    (void)frames_read;  // Unused parameter (reserved for future use)

    // Print diagnostic header
    Log::enqueue(Log::INFO, "=== SIGNAL LEVEL DIAGNOSTIC ===");

    // ADC input level (baseline measurement)
    Log::enqueuef(Log::INFO, "ADC Peak: %d (%.1f%%)", peak_adc,
                  (peak_adc / 2147483647.0f) * 100.0f);

    // Post-pre-emphasis level and gain
    Log::enqueuef(Log::INFO, "After Pre: %d (%.1f%%)  Pre Gain: %.2f dB", peak_pre,
                  (peak_pre / 2147483647.0f) * 100.0f, pre_db);

    // Post-FIR level and total gain
    Log::enqueuef(Log::INFO, "After FIR: %d (%.1f%%)  Total Gain: %.2f dB", peak_fir,
                  (peak_fir / 2147483647.0f) * 100.0f, total_db);
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
