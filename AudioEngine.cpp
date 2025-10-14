#include "AudioEngine.h"
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

namespace
{
    constexpr i2s_port_t kI2SPortTx = I2S_NUM_1;
    constexpr i2s_port_t kI2SPortRx = I2S_NUM_0;
} // namespace

AudioEngine::AudioEngine()
    : pilot_19k_(19000.0f, static_cast<float>(AudioConfig::SAMPLE_RATE_DAC)),
      mpx_synth_(AudioConfig::PILOT_AMP, AudioConfig::DIFF_AMP)
{
    stats_.reset();
}

bool AudioEngine::begin()
{
    Log::enqueue(Log::INFO, "ESP32-S3 Audio DSP: 48kHz -> 192kHz");

    Diagnostics::verifySIMD();

    if (!AudioIO::setupTx())
    {
        Log::enqueue(Log::ERROR, "TX initialization failed!");
        return false;
    }

    delay(100); // allow MCLK to stabilise

    if (!AudioIO::setupRx())
    {
        Log::enqueue(Log::ERROR, "RX initialization failed!");
        return false;
    }

    preemphasis_.configure(AudioConfig::PREEMPHASIS_ALPHA, AudioConfig::PREEMPHASIS_GAIN);
    notch_.configure(static_cast<float>(AudioConfig::SAMPLE_RATE_ADC),
                     AudioConfig::NOTCH_FREQUENCY_HZ, AudioConfig::NOTCH_RADIUS);
    upsampler_.initialize();
    // Configure RDS synthesizer if enabled (assembler task started in setup)
    if (AudioConfig::RDS_ENABLE)
    {
        rds_synth_.configure(static_cast<float>(AudioConfig::SAMPLE_RATE_DAC));
    }

    stats_.reset();
    uint64_t now         = esp_timer_get_time();
    stats_.start_time_us = now;
    stats_.last_print_us = now;

    // Initialize task stats sampling (no-op if unsupported)
    TaskStats::init();

    Log::enqueue(Log::INFO, "System Ready - Starting Audio Processing");

    return true;
}

void AudioEngine::process()
{
    using namespace AudioConfig;

    // Read a block of stereo frames from the ADC (I2S RX).
    // This call blocks until a full DMA chunk is ready.
    size_t    bytes_read = 0;
    esp_err_t ret =
        i2s_read(kI2SPortRx, rx_buffer_, sizeof(rx_buffer_), &bytes_read, portMAX_DELAY);
    if (ret != ESP_OK)
    {
        Log::enqueuef(Log::ERROR, "Read error: %d", ret);
        ++stats_.errors;
        return; // Skip this cycle; keep pipeline deterministic
    }

    if (bytes_read == 0)
    {
        return; // No data yet
    }

    // Each stereo frame is 2 samples * 4 bytes (32‑bit container for 24‑bit audio)
    std::size_t frames_read = bytes_read / (2 * static_cast<std::size_t>(BYTES_PER_SAMPLE));
    if (frames_read == 0)
    {
        return;
    }

    // Capture a timestamp in CPU cycles to measure stage latencies.
    uint32_t cpu_mhz = getCpuFrequencyMhz();
    uint32_t t_start = ESP.getCycleCount();

    // ---------------------------------------------------------------------
    // Stage 1: Convert I2S Q31 container → float [-1, 1]
    // Also gather per-channel peak/RMS for VU metering (pre-filter tap).
    // ---------------------------------------------------------------------
    uint32_t t0       = t_start;
    float    l_sum_sq = 0.0f;
    float    r_sum_sq = 0.0f;
    float    l_peak   = 0.0f;
    float    r_peak   = 0.0f;
    for (std::size_t f = 0; f < frames_read; ++f)
    {
        // Interleaved stereo: L at even index, R at odd index
        std::size_t iL = (f * 2) + 0;
        std::size_t iR = (f * 2) + 1;
        float       vl = static_cast<float>(rx_buffer_[iL]) / Q31_FULL_SCALE;
        float       vr = static_cast<float>(rx_buffer_[iR]) / Q31_FULL_SCALE;
        rx_f32_[iL]    = vl;
        rx_f32_[iR]    = vr;
        l_sum_sq += vl * vl;
        r_sum_sq += vr * vr;
        l_peak = std::max(l_peak, std::fabs(vl));
        r_peak = std::max(r_peak, std::fabs(vr));
    }
    float l_rms     = (frames_read > 0) ? sqrtf(l_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    float r_rms     = (frames_read > 0) ? sqrtf(r_sum_sq / static_cast<float>(frames_read)) : 0.0f;
    float input_rms = (l_rms + r_rms) * 0.5f;

    uint32_t t1        = ESP.getCycleCount();
    uint32_t stage1_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_int_to_float.update(stage1_us);

    // Throttle VU updates to reduce cross-core queue traffic
    {
        static uint64_t s_last_vu_us = 0;
        uint64_t        now_us       = esp_timer_get_time();
        if ((now_us - s_last_vu_us) >= AudioConfig::VU_UPDATE_INTERVAL_US)
        {
            s_last_vu_us = now_us;
            VUMeter::Sample vu{};
            vu.l_rms  = l_rms;
            vu.r_rms  = r_rms;
            vu.l_peak = l_peak;
            vu.r_peak = r_peak;
            vu.l_dbfs = (l_rms > 0.0f) ? (20.0f * log10f(std::min(l_peak, AudioConfig::DBFS_REF) /
                                                         AudioConfig::DBFS_REF))
                                       : -120.0f;
            vu.r_dbfs = (r_rms > 0.0f) ? (20.0f * log10f(std::min(r_peak, AudioConfig::DBFS_REF) /
                                                         AudioConfig::DBFS_REF))
                                       : -120.0f;
            vu.frames = static_cast<uint32_t>(frames_read);
            vu.ts_us  = static_cast<uint32_t>(now_us & 0xFFFFFFFFu);
            VUMeter::enqueue(vu);
        }
    }

    // ---------------------------------------------------------------------
    // Stage 2: 50 µs pre‑emphasis (first‑order high‑pass in discrete form)
    // ---------------------------------------------------------------------
    t0 = t1;
    preemphasis_.process(rx_f32_, frames_read);
    t1                 = ESP.getCycleCount();
    uint32_t stage2_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_preemphasis.update(stage2_us);

    // ---------------------------------------------------------------------
    // Stage 2b: 19 kHz notch biquad (protect stereo pilot region)
    // ---------------------------------------------------------------------
    t0 = t1;
    notch_.process(rx_f32_, frames_read);
    t1                      = ESP.getCycleCount();
    uint32_t stage_notch_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_notch.update(stage_notch_us);

    // ---------------------------------------------------------------------
    // Stage 3: 4× polyphase FIR upsample (48 kHz → 192 kHz)
    // ---------------------------------------------------------------------
    t0 = t1;
    upsampler_.process(rx_f32_, tx_f32_, frames_read);
    t1                 = ESP.getCycleCount();
    uint32_t stage3_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_upsample.update(stage3_us);

    // ---------------------------------------------------------------------
    // Stage 4: Stereo matrix in 192 kHz domain (produce mono=L+R and diff=L‑R)
    // ---------------------------------------------------------------------
    t0                  = t1;
    std::size_t samples = frames_read * UPSAMPLE_FACTOR; // 192 kHz sample count per channel
    stereo_matrix_.process(tx_f32_, mono_buffer_, diff_buffer_, samples);
    t1                       = ESP.getCycleCount();
    uint32_t stage_matrix_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_matrix.update(stage_matrix_us);

    // ---------------------------------------------------------------------
    // Stage 5: Build MPX = mono + pilot*amp + diff*subcarrier*amp (coherent harmonics)
    // ---------------------------------------------------------------------
    t0 = t1;
    // Generate coherent pilot (19k), subcarrier (38k), and 57k carrier from a single master phase
    pilot_19k_.generate_harmonics(pilot_buffer_, subcarrier_buffer_, carrier57_buffer_, samples);
    // Build MPX using the generated pilot/subcarrier
    mpx_synth_.process(mono_buffer_, diff_buffer_, pilot_buffer_, subcarrier_buffer_, mpx_buffer_,
                       samples);
    // Optional: RDS injection at 57 kHz (measure separately)
    if (AudioConfig::RDS_ENABLE)
    {
        uint32_t t_r0 = ESP.getCycleCount();
        rds_synth_.processBlockWithCarrier(carrier57_buffer_, AudioConfig::RDS_AMP, rds_buffer_,
                                           samples);
        for (std::size_t i = 0; i < samples; ++i)
        {
            mpx_buffer_[i] += rds_buffer_[i];
        }
        uint32_t t_r1   = ESP.getCycleCount();
        uint32_t rds_us = (t_r1 - t_r0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
        stats_.stage_rds.update(rds_us);
    }
    // Duplicate MPX to both L/R channels for DAC output
    for (std::size_t i = 0; i < samples; ++i)
    {
        float mpx          = mpx_buffer_[i];
        tx_f32_[i * 2 + 0] = mpx;
        tx_f32_[i * 2 + 1] = mpx;
    }
    t1                    = ESP.getCycleCount();
    uint32_t stage_mpx_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_mpx.update(stage_mpx_us);

    // ---------------------------------------------------------------------
    // Stage 6: Convert float back to int32 (I2S container), clamp near full‑scale
    // ---------------------------------------------------------------------
    t0                        = t1;
    float       sum_output_sq = 0.0f;
    std::size_t out_samples   = frames_read * UPSAMPLE_FACTOR * 2; // stereo interleaved
    for (std::size_t i = 0; i < out_samples; ++i)
    {
        float v = tx_f32_[i];
        // Single clamp location in the pipeline for linearity elsewhere
        v = std::min(0.9999999f, std::max(-1.0f, v));
        sum_output_sq += v * v;
        tx_buffer_[i] = static_cast<int32_t>(v * 2147483647.0f);
    }
    float output_rms = 0.0f;
    if (out_samples > 0)
    {
        output_rms = sqrtf(sum_output_sq / static_cast<float>(out_samples));
    }

    t1                 = ESP.getCycleCount();
    uint32_t stage4_us = (t1 - t0) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.stage_float_to_int.update(stage4_us);

    // No longer compute or report overall gain in logs

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    int32_t peak_adc = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);
#endif

    uint32_t t_end    = t1;
    uint32_t total_us = (t_end - t_start) / (cpu_mhz ? cpu_mhz : static_cast<uint32_t>(240));
    stats_.total.update(total_us);

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    int32_t peak_after_pre = 0;
    for (std::size_t i = 0; i < frames_read * 2; ++i)
    {
        float v       = rx_f32_[i];
        v             = std::min(0.9999999f, std::max(-1.0f, v));
        rx_buffer_[i] = static_cast<int32_t>(v * 2147483647.0f);
    }
    peak_after_pre = Diagnostics::findPeakAbs(rx_buffer_, frames_read * 2);

    int32_t peak_after_fir =
        Diagnostics::findPeakAbs(tx_buffer_, frames_read * UPSAMPLE_FACTOR * 2);

    ++diagnostic_counter_;
    if (diagnostic_counter_ >= DIAGNOSTIC_PRINT_INTERVAL)
    {
        diagnostic_counter_ = 0;
        float pre_db =
            20.0f * log10f(static_cast<float>(peak_after_pre) / static_cast<float>(peak_adc));
        float total_db =
            20.0f * log10f(static_cast<float>(peak_after_fir) / static_cast<float>(peak_adc));
        printDiagnostics(frames_read, peak_adc, peak_after_pre, peak_after_fir, pre_db, total_db);
    }
#endif

    // Write to DAC
    size_t bytes_to_write = frames_read * UPSAMPLE_FACTOR * 2 * BYTES_PER_SAMPLE;
    size_t bytes_written  = 0;
    ret = i2s_write(kI2SPortTx, tx_buffer_, bytes_to_write, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK)
    {
        Log::enqueuef(Log::ERROR, "Write error: %d", ret);
        ++stats_.errors;
    }

    if (bytes_written != bytes_to_write)
    {
        Log::enqueuef(Log::WARN, "Underrun (wrote %u/%u bytes)", (unsigned)bytes_written,
                      (unsigned)bytes_to_write);
    }

    ++stats_.loops_completed;

    uint64_t now = esp_timer_get_time();
    if (now - stats_.last_print_us >= STATS_PRINT_INTERVAL_US)
    {
        stats_.last_print_us = now;
        float available_us   = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);
        float cpu_usage =
            (available_us > 0.0f) ? (stats_.total.current / available_us * 100.0f) : 0.0f;
        float cpu_headroom = 100.0f - cpu_usage;
        printPerformance(frames_read, available_us, cpu_usage, cpu_headroom);
    }

    // Send a compact status snapshot to the VU task for on-screen status panel
    {
        static uint64_t s_last_status_us = 0;
        if ((now - s_last_status_us) >= AudioConfig::STATUS_PANEL_UPDATE_US)
        {
            s_last_status_us = now;
            // Recompute CPU usage/headroom here (independent of 5s log gate)
            float available_us = (frames_read * 1'000'000.0f) / static_cast<float>(SAMPLE_RATE_ADC);
            float cpu_usage =
                (available_us > 0.0f) ? (stats_.total.current / available_us * 100.0f) : 0.0f;
            float                  cpu_headroom = 100.0f - cpu_usage;
            VUMeter::StatsSnapshot snap{};
            snap.cpu_usage     = cpu_usage;
            snap.cpu_headroom  = cpu_headroom;
            snap.total_us_cur  = static_cast<float>(stats_.total.current);
            snap.total_us_min  = static_cast<float>(stats_.total.min);
            snap.total_us_max  = static_cast<float>(stats_.total.max);
            snap.fir_us_cur    = static_cast<float>(stats_.stage_upsample.current);
            snap.mpx_us_cur    = static_cast<float>(stats_.stage_mpx.current);
            snap.matrix_us_cur = static_cast<float>(stats_.stage_matrix.current);
            snap.rds_us_cur    = static_cast<float>(stats_.stage_rds.current);
            snap.heap_free     = ESP.getFreeHeap();
            snap.heap_min      = ESP.getMinFreeHeap();
            snap.uptime_s =
                static_cast<uint32_t>((esp_timer_get_time() - stats_.start_time_us) / 1000000ULL);
            snap.loops_completed = stats_.loops_completed;
            snap.errors          = stats_.errors;

            // Optional: per-core and per-task CPU (if run-time stats enabled)
            float    core0 = 0, core1 = 0, aud = 0, logg = 0, vu = 0;
            uint32_t a_sw = 0, l_sw = 0, v_sw = 0;
            bool     cpu_ok = TaskStats::collect(core0, core1, aud, logg, vu, a_sw, l_sw, v_sw);
            if (cpu_ok)
            {
                snap.core0_load              = core0;
                snap.core1_load              = core1;
                snap.audio_cpu               = aud;
                snap.logger_cpu              = logg;
                snap.vu_cpu                  = vu;
                snap.audio_stack_free_words  = a_sw;
                snap.logger_stack_free_words = l_sw;
                snap.vu_stack_free_words     = v_sw;
                snap.cpu_valid               = 1;
            }
            else
            {
                snap.cpu_valid = 0;
            }
            VUMeter::enqueueStats(snap);
        }
    }
}

bool AudioEngine::startTaskInstance(int core_id, UBaseType_t priority, uint32_t stack_words)
{
    BaseType_t ok = xTaskCreatePinnedToCore(taskTrampoline, "audio", stack_words, this, priority,
                                            &task_handle_, core_id);
    return ok == pdPASS;
}

void AudioEngine::taskTrampoline(void* arg)
{
    auto* self = static_cast<AudioEngine*>(arg);
    if (!self->begin())
    {
        Log::enqueue(Log::ERROR, "AudioEngine begin() failed");
        vTaskDelete(nullptr);
        return;
    }
    for (;;)
    {
        self->process();
    }
}

// Static facade that manages a singleton AudioEngine
bool AudioEngine::startTask(int core_id, UBaseType_t priority, uint32_t stack_words)
{
    static AudioEngine s_instance;
    return s_instance.startTaskInstance(core_id, priority, stack_words);
}

void AudioEngine::printPerformance(std::size_t frames_read, float available_us, float cpu_usage,
                                   float cpu_headroom)
{
    using namespace AudioConfig;

    (void)frames_read;

    // Header
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

void AudioEngine::printDiagnostics(std::size_t frames_read, int32_t peak_adc, int32_t peak_pre,
                                   int32_t peak_fir, float pre_db, float total_db)
{
    using namespace AudioConfig;

    (void)frames_read;

    Log::enqueue(Log::INFO, "=== SIGNAL LEVEL DIAGNOSTIC ===");
    Log::enqueuef(Log::INFO, "ADC Peak: %d (%.1f%%)", peak_adc,
                  (peak_adc / 2147483647.0f) * 100.0f);
    Log::enqueuef(Log::INFO, "After Pre: %d (%.1f%%)  Pre Gain: %.2f dB", peak_pre,
                  (peak_pre / 2147483647.0f) * 100.0f, pre_db);
    Log::enqueuef(Log::INFO, "After FIR: %d (%.1f%%)  Total Gain: %.2f dB", peak_fir,
                  (peak_fir / 2147483647.0f) * 100.0f, total_db);
}
