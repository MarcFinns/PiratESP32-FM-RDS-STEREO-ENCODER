/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
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
#include "ModuleBase.h"
#include "ErrorHandler.h"
#include "Log.h"
#include "RDSSynth.h"
#include "TaskStats.h"
#include "VUMeter.h"

#include <Arduino.h>
#include <esp32-hal-cpu.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <esp_err.h>

#include <algorithm>
#include <cmath>

// ==================================================================================
//                          CONSTRUCTOR
// ==================================================================================

/**
 * DSP_pipeline Constructor
 *
 * Initializes the audio processing pipeline with default settings and
 * injected hardware driver dependency.
 * All buffers are zero-initialized by default (BSS section).
 *
 * Parameters:
 *   hardware_driver:  Pointer to hardware I/O driver implementation
 *                     (e.g., ESP32I2SDriver instance)
 *
 * Initialization:
 *   • pilot_19k_: NCO configured for 19 kHz pilot at 192 kHz sample rate
 *   • mpx_synth_: MPX mixer with pilot and stereo difference amplitudes
 *   • stats_: Performance statistics tracker (reset to zero)
 *   • hardware_driver_: Stored for use in begin() and process()
 *
 * Note: Hardware I2S interfaces are NOT configured here. Call begin() to
 *       initialize the hardware driver and load DSP filter coefficients.
 */
DSP_pipeline::DSP_pipeline(IHardwareDriver *hardware_driver)
    : hardware_driver_(hardware_driver),
      pilot_19k_(19000.0f, static_cast<float>(Config::SAMPLE_RATE_DAC)),
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
    Log::enqueue(LogLevel::INFO, "ESP32-S3 Audio DSP: 48kHz -> 192kHz");
    Log::enqueuef(LogLevel::INFO, "DSP_pipeline running on Core %d", xPortGetCoreID());

    // Verify SIMD/ESP32-S3 specific optimizations are available
    Diagnostics::verifySIMD();

    // Verify hardware I/O is ready (initialized by SystemContext)
    // SystemContext::initialize() is responsible for calling hardware_driver_->initialize().
    // Avoid double-initialization here to prevent redundant driver setup.
    if (!hardware_driver_ || !hardware_driver_->isReady())
    {
        Log::enqueue(LogLevel::ERROR, "Hardware driver not ready (initialize via SystemContext first)");
        return false;
    }

    // Configure pre-emphasis filter (50 µs for FM broadcast standard)
    // Alpha: filter coefficient, Gain: compensation for high-frequency boost
    preemphasis_.configure(Config::PREEMPHASIS_ALPHA, Config::PREEMPHASIS_GAIN);

    // Configure 19 kHz notch filter to prevent pilot tone interference
    // Removes any audio content at the pilot frequency
    notch_.configure(static_cast<float>(Config::SAMPLE_RATE_ADC), Config::NOTCH_FREQUENCY_HZ,
                     Config::NOTCH_RADIUS);

    // Initialize polyphase FIR upsampler (loads filter coefficients from flash)
    upsampler_.initialize();

    // Configure RDS synthesizer if RDS transmission is enabled
    // RDS assembler task must be started separately in setup()
    if (Config::ENABLE_RDS_57K)
    {
        rds_synth_.configure(static_cast<float>(Config::SAMPLE_RATE_DAC));
    }

    // Reset performance statistics and start timing
    stats_.reset();
    uint64_t now = esp_timer_get_time();
    stats_.start_time_us = now; // Record system start time
    stats_.last_print_us = now; // Initialize performance logging timer

    // Initialize FreeRTOS task statistics monitoring (if enabled in menuconfig)
    TaskStats::init();

    // Initialization complete, ready to process audio
    Log::enqueue(LogLevel::INFO, "System Ready - Starting Audio Processing");

    return true;
}

// ==================================================================================
//                    ORCHESTRATION HELPER METHODS (STAGE EXTRACTION)
// ==================================================================================

/**
 * Read and convert audio from ADC with VU level calculation
 *
 * Encapsulates Stage 1: I2S read + int→float conversion + VU metrics
 */
bool DSP_pipeline::readAndConvertAudio(std::size_t &frames_read, float &l_peak, float &r_peak,
                                       float &l_rms, float &r_rms, uint32_t &rx_wait_us,
                                       uint32_t cpu_mhz, uint32_t &deint_us)
{
    using namespace Config;

    size_t bytes_read = 0;
    uint32_t tr0 = ESP.getCycleCount();
    if (!hardware_driver_->read(rx_buffer_, sizeof(rx_buffer_), bytes_read,
                                Config::I2S_READ_TIMEOUT_MS))
    {
        auto derr = hardware_driver_->getLastError();
        int  perr = hardware_driver_->getErrorStatus();
        const char* err_name = esp_err_to_name(static_cast<esp_err_t>(perr));
        char details[96];
        switch (derr)
        {
        case DriverError::Timeout:
            snprintf(details, sizeof(details), "I2S RX timeout (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::TIMEOUT, "DSP_pipeline::readAndConvertAudio", details);
            break;
        case DriverError::InvalidArgument:
            snprintf(details, sizeof(details), "I2S RX invalid arg (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::INVALID_PARAM, "DSP_pipeline::readAndConvertAudio", details);
            break;
        case DriverError::InvalidState:
        case DriverError::NotInitialized:
            snprintf(details, sizeof(details), "I2S RX not ready (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::I2S_NOT_INITIALIZED, "DSP_pipeline::readAndConvertAudio", details);
            break;
        default:
            snprintf(details, sizeof(details), "I2S RX error (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::I2S_READ_ERROR, "DSP_pipeline::readAndConvertAudio", details);
            break;
        }
        ++stats_.errors;
        return false;
    }
    uint32_t tr1 = ESP.getCycleCount();
    rx_wait_us = (tr1 - tr0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));

    if (bytes_read == 0)
        return false;

    frames_read = bytes_read / (2 * static_cast<std::size_t>(BYTES_PER_SAMPLE));
    if (frames_read == 0)
        return false;

    // Convert Q31 to float and calculate VU levels
    uint32_t tc0 = ESP.getCycleCount();
    float l_sum_sq = 0.0f, r_sum_sq = 0.0f;
    l_peak = 0.0f;
    r_peak = 0.0f;

    for (std::size_t f = 0; f < frames_read; ++f)
    {
        std::size_t iL = (f * 2) + 0;
        std::size_t iR = (f * 2) + 1;

        float vl = static_cast<float>(rx_buffer_[iL]) / Q31_FULL_SCALE;
        float vr = static_cast<float>(rx_buffer_[iR]) / Q31_FULL_SCALE;

        if (!Config::ENABLE_AUDIO)
        {
            vl = 0.0f;
            vr = 0.0f;
        }

        rx_f32_[iL] = vl;
        rx_f32_[iR] = vr;

        l_sum_sq += vl * vl;
        r_sum_sq += vr * vr;
        l_peak = std::max(l_peak, std::fabs(vl));
        r_peak = std::max(r_peak, std::fabs(vr));
    }

    l_rms = (frames_read > 0) ? sqrtf(l_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    r_rms = (frames_read > 0) ? sqrtf(r_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    uint32_t tc1 = ESP.getCycleCount();
    deint_us = (tc1 - tc0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));

    return true;
}

/**
 * Update VU meters with audio levels (throttled to 5 ms)
 */
void DSP_pipeline::updateVUMeters(float l_peak, float r_peak, float l_rms, float r_rms,
                                  std::size_t frames_read)
{
    static uint64_t s_last_vu_us = 0;
    uint64_t now_us = esp_timer_get_time();

    if ((now_us - s_last_vu_us) < Config::VU_UPDATE_INTERVAL_US)
        return;

    s_last_vu_us = now_us;

    VUSample vu{};
    vu.l_rms = l_rms;
    vu.r_rms = r_rms;
    vu.l_peak = l_peak;
    vu.r_peak = r_peak;

    vu.l_dbfs = (l_rms > 0.0f)
                    ? (20.0f * log10f(std::min(l_peak, Config::DBFS_REF) / Config::DBFS_REF))
                    : -120.0f;
    vu.r_dbfs = (r_rms > 0.0f)
                    ? (20.0f * log10f(std::min(r_peak, Config::DBFS_REF) / Config::DBFS_REF))
                    : -120.0f;

    vu.frames = static_cast<uint32_t>(frames_read);
    vu.ts_us = static_cast<uint32_t>(now_us & 0xFFFFFFFFu);

    VUMeter::enqueue(vu);
}

/**
 * Convert float samples to int32 Q31 (conversion only, no I/O)
 *
 * Optimized for performance: simplified direct conversion without intermediate
 * accumulation or nested min/max operations. The DSP pipeline upstream ensures
 * output levels stay within [-1.0, 1.0] via proper gain staging, so additional
 * clipping here is redundant.
 *
 * Performance: ~30-50 µs for 512 samples
 */
void DSP_pipeline::convertFloatToInt32(std::size_t frames_read)
{
    using namespace Config;

    std::size_t out_samples = frames_read * UPSAMPLE_FACTOR * 2;

    // Optimized conversion: direct float→int cast with Q31 scaling
    // No intermediate accumulation (sum_output_sq was unused dead code)
    // Compiler can vectorize this simple loop with SIMD
    constexpr float Q31_SCALE = 2147483647.0f; // 2^31 - 1
    for (std::size_t i = 0; i < out_samples; ++i)
    {
        float v = tx_f32_[i];
        // Clamp to [-1.0, 1.0] to prevent overflow (minimal cost)
        if (v > 1.0f)
            v = 1.0f;
        else if (v < -1.0f)
            v = -1.0f;
        tx_buffer_[i] = static_cast<int32_t>(v * Q31_SCALE);
    }
}

/**
 * Write samples to I2S DAC
 *
 * Separated from conversion to properly measure I/O blocking time vs. computation.
 */
void DSP_pipeline::writeToDAC(std::size_t frames_read)
{
    using namespace Config;

    size_t bytes_to_write = frames_read * UPSAMPLE_FACTOR * 2 * BYTES_PER_SAMPLE;
    size_t bytes_written = 0;

    if (!hardware_driver_->write(tx_buffer_, bytes_to_write, bytes_written,
                                 Config::I2S_WRITE_TIMEOUT_MS))
    {
        auto derr = hardware_driver_->getLastError();
        int  perr = hardware_driver_->getErrorStatus();
        const char* err_name = esp_err_to_name(static_cast<esp_err_t>(perr));
        char details[96];
        switch (derr)
        {
        case DriverError::Timeout:
            snprintf(details, sizeof(details), "I2S TX timeout (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::TIMEOUT, "DSP_pipeline::writeToDAC", details);
            break;
        case DriverError::InvalidArgument:
            snprintf(details, sizeof(details), "I2S TX invalid arg (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::INVALID_PARAM, "DSP_pipeline::writeToDAC", details);
            break;
        case DriverError::InvalidState:
        case DriverError::NotInitialized:
            snprintf(details, sizeof(details), "I2S TX not ready (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::I2S_NOT_INITIALIZED, "DSP_pipeline::writeToDAC", details);
            break;
        default:
            snprintf(details, sizeof(details), "I2S TX error (esp:%s)", err_name);
            ErrorHandler::logError(ErrorCode::I2S_WRITE_ERROR, "DSP_pipeline::writeToDAC", details);
            break;
        }
        ++stats_.errors;
    }

    if (bytes_written != bytes_to_write)
    {
        Log::enqueuef(LogLevel::WARN, "Underrun (wrote %u/%u bytes)", (unsigned)bytes_written,
                      (unsigned)bytes_to_write);
    }

    ++stats_.loops_completed;
}

/**
 * Update performance metrics and status (throttled to 5s / 1s)
 */
void DSP_pipeline::updatePerformanceMetrics(uint32_t total_us, std::size_t frames_read)
{
    using namespace Config;
    uint64_t now = esp_timer_get_time();

    // Performance logging every 5 seconds
    if (now - stats_.last_print_us >= STATS_PRINT_INTERVAL_US)
    {
        stats_.last_print_us = now;
        float available_us = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);
        float cpu_usage = (available_us > 0.0f) ? (total_us / available_us * 100.0f) : 0.0f;
        float cpu_headroom = 100.0f - cpu_usage;
        printPerformance(frames_read, available_us, cpu_usage, cpu_headroom);
    }

    // Status panel update every 1 second
    {
        static uint64_t s_last_status_us = 0;
        if ((now - s_last_status_us) >= STATUS_PANEL_UPDATE_US)
        {
            s_last_status_us = now;

            float available_us = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);
            float cpu_usage = (available_us > 0.0f) ? (total_us / available_us * 100.0f) : 0.0f;
            float cpu_headroom = 100.0f - cpu_usage;

            VUStatsSnapshot snap{};
            snap.cpu_usage = cpu_usage;
            snap.cpu_headroom = cpu_headroom;
            snap.total_us_cur = static_cast<float>(total_us);
            snap.total_us_min = static_cast<float>(stats_.total.min);
            snap.total_us_max = static_cast<float>(stats_.total.max);
            snap.fir_us_cur = static_cast<float>(stats_.stage_upsample.current);
            snap.mpx_us_cur = static_cast<float>(stats_.stage_mpx.current);
            snap.matrix_us_cur = static_cast<float>(stats_.stage_matrix.current);
            snap.rds_us_cur = static_cast<float>(stats_.stage_rds.current);
            snap.heap_free = ESP.getFreeHeap();
            snap.heap_min = ESP.getMinFreeHeap();
            snap.uptime_s =
                static_cast<uint32_t>((esp_timer_get_time() - stats_.start_time_us) / 1000000ULL);
            snap.loops_completed = stats_.loops_completed;
            snap.errors = stats_.errors;

            float core0 = 0, core1 = 0, aud = 0, logg = 0, vu = 0;
            uint32_t a_sw = 0, l_sw = 0, v_sw = 0;
            if (TaskStats::collect(core0, core1, aud, logg, vu, a_sw, l_sw, v_sw))
            {
                snap.core0_load = core0;
                snap.core1_load = core1;
                snap.audio_cpu = aud;
                snap.logger_cpu = logg;
                snap.vu_cpu = vu;
                snap.audio_stack_free_words = a_sw;
                snap.logger_stack_free_words = l_sw;
                snap.vu_stack_free_words = v_sw;
                snap.cpu_valid = 1;
            }
            else
            {
                snap.cpu_valid = 0;
            }

            VUMeter::enqueueStats(snap);
        }
    }
}

// ==================================================================================
//                          MAIN AUDIO PROCESSING LOOP (process)
// ==================================================================================

/**
 * Process One Audio Block (Orchestration-Only)
 *
 * Simplified main processing loop that orchestrates the 8-stage DSP pipeline.
 * All detailed implementation moved to helper methods.
 *
 * Processing Pipeline (8 stages):
 *   1. I2S Read + Convert:  Read 64 stereo samples from ADC, convert to float
 *   2. Pre-emphasis:        Apply 50 µs FM pre-emphasis filter (optional)
 *   3. Notch Filter:        Remove 19 kHz content (pilot tone protection)
 *   4. Upsample:            48 kHz → 192 kHz using polyphase FIR (15 kHz LPF)
 *   5. Stereo Matrix:       Generate L+R (mono) and L-R (diff) signals
 *   6. MPX Synthesis:       Mix mono + pilot + (diff × 38 kHz) + RDS
 *   7. DAC Convert + Write: Convert to Q31, soft clip, write to DAC
 *   8. Metrics:             Update VU meters, performance logs
 *
 * Real-Time Constraints:
 *   • Must complete in < 1.33 ms
 *   • Typical: ~300 µs (22% CPU usage)
 *   • No dynamic allocation in process()
 */
void DSP_pipeline::process()
{
    using namespace Config;

    uint32_t cpu_mhz = getCpuFrequencyMhz(); // Typically 240 MHz
    uint32_t t_start = ESP.getCycleCount();  // Start of processing

    // ============================================================================
    // STAGE 1: I2S Read + Int→Float Conversion + VU Calculation
    // ============================================================================
    std::size_t frames_read = 0;
    float l_peak = 0.0f, r_peak = 0.0f, l_rms = 0.0f, r_rms = 0.0f;

    uint32_t rx_wait_us_local = 0;
    uint32_t deint_us_local = 0;
    if (!readAndConvertAudio(frames_read, l_peak, r_peak, l_rms, r_rms,
                             rx_wait_us_local, cpu_mhz, deint_us_local))
    {
        return; // Skip cycle on read error or no data
    }

    uint32_t t0 = ESP.getCycleCount();
    // Update refined timings
    stats_.stage_i2s_rx_wait.update(rx_wait_us_local);
    stats_.stage_int_to_float.update(deint_us_local);

    updateVUMeters(l_peak, r_peak, l_rms, r_rms, frames_read);

    // ============================================================================
    // STAGE 2: FM Pre-Emphasis Filter (Optional)
    // ============================================================================
    uint32_t t1 = t0;
    if (Config::ENABLE_PREEMPHASIS)
    {
        t0 = t1;
        preemphasis_.process(rx_f32_, frames_read);
        t1 = ESP.getCycleCount();
        uint32_t stage2_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
        stats_.stage_preemphasis.update(stage2_us);
    }

    // ============================================================================
    // STAGE 3: 19 kHz Notch Filter
    // ============================================================================
    t0 = t1;
    notch_.process(rx_f32_, frames_read);
    t1 = ESP.getCycleCount();
    uint32_t stage_notch_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_notch.update(stage_notch_us);

    // ============================================================================
    // STAGE 4: Polyphase FIR Upsampler (48 kHz → 192 kHz)
    // ============================================================================
    t0 = t1;
    upsampler_.process(rx_f32_, tx_f32_, frames_read);
    t1 = ESP.getCycleCount();
    uint32_t stage3_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_upsample.update(stage3_us);

    // ============================================================================
    // STAGE 5: Stereo Matrix (L+R and L-R Generation)
    // ============================================================================
    t0 = t1;
    std::size_t samples = frames_read * Config::UPSAMPLE_FACTOR;
    stereo_matrix_.process(tx_f32_, mono_buffer_, diff_buffer_, samples);
    t1 = ESP.getCycleCount();
    uint32_t stage_matrix_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_matrix.update(stage_matrix_us);

    // ============================================================================
    // STAGE 6: FM Multiplex (MPX) Synthesis + RDS Injection
    // ============================================================================
    t0 = t1;

    // Generate phase-coherent carriers (19 kHz pilot, 38 kHz subcarrier, 57 kHz RDS)
    bool need19 = Config::ENABLE_STEREO_PILOT_19K;
    bool need38 = (Config::ENABLE_AUDIO && Config::ENABLE_STEREO_SUBCARRIER_38K);
    bool need57 = Config::ENABLE_RDS_57K;
    if (need19 || need38 || need57)
    {
        pilot_19k_.generate_harmonics(pilot_buffer_, subcarrier_buffer_, carrier57_buffer_,
                                      samples);
    }

    // Mix mono + pilot + stereo subcarrier
    mpx_synth_.process(mono_buffer_, diff_buffer_, pilot_buffer_, subcarrier_buffer_, mpx_buffer_,
                       samples);

    // Inject RDS if enabled
    if (Config::ENABLE_RDS_57K)
    {
        uint32_t t_r0 = ESP.getCycleCount();
        rds_synth_.processBlockWithCarrier(carrier57_buffer_, Config::RDS_AMP, rds_buffer_,
                                           samples);

        for (std::size_t i = 0; i < samples; ++i)
        {
            mpx_buffer_[i] += rds_buffer_[i];
        }

        uint32_t t_r1 = ESP.getCycleCount();
        uint32_t rds_us = (t_r1 - t_r0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
        stats_.stage_rds.update(rds_us);
    }

    // Duplicate MPX to stereo DAC channels
    for (std::size_t i = 0; i < samples; ++i)
    {
        float mpx = mpx_buffer_[i];
        tx_f32_[i * 2 + 0] = mpx;
        tx_f32_[i * 2 + 1] = mpx;
    }

    t1 = ESP.getCycleCount();
    uint32_t stage_mpx_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_mpx.update(stage_mpx_us);

    // ============================================================================
    // STAGE 7: Float→Int Conversion (Computation Only)
    // ============================================================================
    t0 = t1;
    convertFloatToInt32(frames_read);
    t1 = ESP.getCycleCount();
    uint32_t stage4_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_float_to_int.update(stage4_us);

    // ============================================================================
    // STAGE 7b: I2S Write (Separate Measurement - Shows I/O Blocking Time)
    // ============================================================================
    // Note: This is NOT included in stage_float_to_int timing to show pure conversion cost
    writeToDAC(frames_read);

    // ============================================================================
    // STAGE 8: Performance Monitoring & Metrics Update
    // ============================================================================
    uint32_t t_end = t1;
    uint32_t total_us = (t_end - t_start) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.total.update(total_us);

    updatePerformanceMetrics(total_us, frames_read);

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    // Optional diagnostic logging for signal analysis
    int32_t peak_adc = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);

    // Convert pre-emphasis output back to int32 to measure peak level
    int32_t peak_after_pre = 0;
    for (std::size_t i = 0; i < frames_read * 2; ++i)
    {
        float v = rx_f32_[i];
        v = std::min(0.9999999f, std::max(-1.0f, v));
        rx_buffer_[i] = static_cast<int32_t>(v * 2147483647.0f);
    }
    peak_after_pre = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);

    // Measure peak level after FIR upsampler
    int32_t peak_after_fir =
        Diagnostics::findPeakAbs(tx_buffer_, frames_read * Config::UPSAMPLE_FACTOR * 2);

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
    return ModuleBase::spawnTaskFor(
        this,
        "audio",
        stack_words,
        priority,
        core_id,
        DSP_pipeline::taskTrampoline,
        &task_handle_);
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
void DSP_pipeline::taskTrampoline(void *arg)
{
    // Cast void* parameter to DSP_pipeline* instance
    auto *self = static_cast<DSP_pipeline *>(arg);

    // Initialize hardware and DSP modules
    if (!self->begin())
    {
        Log::enqueue(LogLevel::ERROR, "DSP_pipeline begin() failed");
        vTaskDelete(nullptr); // Exit task on initialization failure
        return;
    }

    // Infinite audio processing loop (never returns)
    for (;;)
    {
        self->process(); // Process one audio block (~1.33 ms @ 48 kHz)
    }
}

/**
 * Start Audio Task (Static with Hardware Driver)
 *
 * Creates a DSP_pipeline instance with injected hardware driver and spawns its task.
 * This is the recommended way to start the audio engine from main code.
 *
 * Parameters:
 *   hardware_driver:  Hardware I/O implementation (e.g., ESP32I2SDriver)
 *   core_id:          CPU core (must be 0 for audio)
 *   priority:         Task priority (typically 6, highest priority)
 *   stack_words:      Stack size in words (typically 12288 = 48 KB)
 *
 * Returns:
 *   true:  Audio task started successfully
 *   false: Task creation failed
 *
 * Note: hardware_driver must remain valid for the lifetime of the audio task.
 *
 * Example Usage:
 *   void setup() {
 *       ESP32I2SDriver driver;
 *       DSP_pipeline::startTask(&driver, 0, 6, 12288);
 *   }
 */
bool DSP_pipeline::startTask(IHardwareDriver *hardware_driver, int core_id, UBaseType_t priority,
                             uint32_t stack_words)
{
    static DSP_pipeline s_instance(hardware_driver); // Create with driver dependency
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

    (void)frames_read; // Unused parameter (reserved for future use)

    // Print formatted header
    Log::enqueue(LogLevel::INFO, "========================================");
    Log::enqueue(LogLevel::INFO, "Performance Stats");
    Log::enqueue(LogLevel::INFO, "========================================");

    // Summary
    Log::enqueuef(LogLevel::INFO, "Loops completed: %u", stats_.loops_completed);
    Log::enqueuef(LogLevel::INFO, "Errors: %u", stats_.errors);
    float uptime_s = (esp_timer_get_time() - stats_.start_time_us) / 1'000'000.0f;
    Log::enqueuef(LogLevel::INFO, "Uptime: %.1f seconds", uptime_s);

    // Processing times
    Log::enqueue(LogLevel::INFO, "----------------------------------------");
    Log::enqueue(LogLevel::INFO, "Processing time:");
    Log::enqueuef(LogLevel::INFO, "  Current: %.2f µs", (double)stats_.total.current);
    Log::enqueuef(LogLevel::INFO, "  Min: %.2f µs", (double)stats_.total.min);
    Log::enqueuef(LogLevel::INFO, "  Max: %.2f µs", (double)stats_.total.max);
    Log::enqueuef(LogLevel::INFO, "  Available: %.2f µs", (double)available_us);
    Log::enqueuef(LogLevel::INFO, "CPU usage: %.1f%%", (double)cpu_usage);
    Log::enqueuef(LogLevel::INFO, "CPU headroom: %.1f%%", (double)cpu_headroom);

    // Per-stage breakdown
    Log::enqueue(LogLevel::INFO, "----------------------------------------");
    Log::enqueue(LogLevel::INFO, "Per-Stage Breakdown:");
    Log::enqueue(LogLevel::INFO, "  1a. I2S RX wait (block):");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_i2s_rx_wait.current, (double)stats_.stage_i2s_rx_wait.min,
                  (double)stats_.stage_i2s_rx_wait.max);
    Log::enqueue(LogLevel::INFO, "  1b. Deinterleave (int→float):");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_int_to_float.current, (double)stats_.stage_int_to_float.min,
                  (double)stats_.stage_int_to_float.max);
    Log::enqueue(LogLevel::INFO, "  2. Gain processing:");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_preemphasis.current, (double)stats_.stage_preemphasis.min,
                  (double)stats_.stage_preemphasis.max);
    Log::enqueue(LogLevel::INFO, "  3. 19 kHz notch:");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_notch.current, (double)stats_.stage_notch.min,
                  (double)stats_.stage_notch.max);
    Log::enqueue(LogLevel::INFO, "  4. Upsample 4× (FIR):");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_upsample.current, (double)stats_.stage_upsample.min,
                  (double)stats_.stage_upsample.max);
    Log::enqueue(LogLevel::INFO, "  5. Stereo matrix:");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_matrix.current, (double)stats_.stage_matrix.min,
                  (double)stats_.stage_matrix.max);
    Log::enqueue(LogLevel::INFO, "  6. MPX synthesis:");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_mpx.current, (double)stats_.stage_mpx.min,
                  (double)stats_.stage_mpx.max);
    Log::enqueue(LogLevel::INFO, "  7. RDS injection:");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_rds.current, (double)stats_.stage_rds.min,
                  (double)stats_.stage_rds.max);
    Log::enqueue(LogLevel::INFO, "  8. Conversion (float→int):");
    Log::enqueuef(LogLevel::INFO, "     Cur: %6.2f µs  Min: %6.2f µs  Max: %6.2f µs",
                  (double)stats_.stage_float_to_int.current, (double)stats_.stage_float_to_int.min,
                  (double)stats_.stage_float_to_int.max);

    // Footer
    Log::enqueue(LogLevel::INFO, "----------------------------------------");
    Log::enqueuef(LogLevel::INFO, "Free heap: %u bytes", (unsigned)ESP.getFreeHeap());
    Log::enqueuef(LogLevel::INFO, "Min free heap: %u bytes", (unsigned)ESP.getMinFreeHeap());
    // Gain reporting removed per request
    Log::enqueue(LogLevel::INFO, "========================================");
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

    (void)frames_read; // Unused parameter (reserved for future use)

    // Print diagnostic header
    Log::enqueue(LogLevel::INFO, "=== SIGNAL LEVEL DIAGNOSTIC ===");

    // ADC input level (baseline measurement)
    Log::enqueuef(LogLevel::INFO, "ADC Peak: %d (%.1f%%)", peak_adc,
                  (peak_adc / 2147483647.0f) * 100.0f);

    // Post-pre-emphasis level and gain
    Log::enqueuef(LogLevel::INFO, "After Pre: %d (%.1f%%)  Pre Gain: %.2f dB", peak_pre,
                  (peak_pre / 2147483647.0f) * 100.0f, pre_db);

    // Post-FIR level and total gain
    Log::enqueuef(LogLevel::INFO, "After FIR: %d (%.1f%%)  Total Gain: %.2f dB", peak_fir,
                  (peak_fir / 2147483647.0f) * 100.0f, total_db);
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
