/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                          Audio Engine Interface
 *
 * =====================================================================================
 *
 * File:         DSP_pipeline.h
 * Description:  Main audio processing pipeline orchestration
 *
 * Purpose:
 *   DSP_pipeline is the central coordinator for the entire FM stereo encoding pipeline.
 *   It manages all DSP stages, I2S communication, and real-time performance monitoring.
 *   Runs as a dedicated FreeRTOS task on Core 0 (real-time audio core).
 *
 * Processing Pipeline:
 *   1. I2S RX:  Read 48 kHz stereo audio from ADC (64 sample pairs = 1.33 ms)
 *   2. Convert: int32 → float32 (Q31 fixed-point to normalized float)
 *   3. Pre-emphasis: Apply 50 µs FM pre-emphasis filter (IIR)
 *   4. Notch: Remove 19 kHz content to prevent pilot tone interference
 *   5. Upsample: 48 kHz → 192 kHz using 4× polyphase FIR filter
 *   6. Matrix: Generate L+R (mono) and L-R (stereo difference) signals
 *   7. NCO: Generate coherent 19/38/57 kHz carriers (harmonics from pilot)
 *   8. MPX: Mix mono + pilot + (L-R × 38 kHz) and inject RDS (57 kHz)
 *   9. Convert: float32 → int32 (normalized float to Q31 fixed-point)
 *  10. I2S TX: Write 192 kHz stereo to DAC (256 sample pairs = 1.33 ms)
 *
 * Real-Time Constraints:
 *   • Block size: 64 samples @ 48 kHz = 1.33 ms available time
 *   • Target CPU: < 30% (leaves 70% headroom for jitter tolerance)
 *   • Typical performance: ~300 µs processing time (~22% CPU usage)
 *   • All processing must complete within 1.33 ms or audio glitches occur
 *
 * Task Architecture:
 *   • Pinned to Core 0 (dedicated audio core, no interruptions)
 *   • Priority 6 (highest priority, preempts all non-critical tasks)
 *   • Stack: 12 KB (DSP buffers + FreeRTOS overhead)
 *   • Infinite loop: Read → Process → Write → Repeat
 *
 * Memory Layout:
 *   All DSP buffers are 16-byte aligned for potential SIMD optimization.
 *   Total buffer memory: ~4 KB per DSP_pipeline instance.
 *
 * Performance Monitoring:
 *   Every 5 seconds, prints to Serial:
 *     • Sample rate
 *     • CPU usage percentage
 *     • Headroom percentage
 *     • Peak audio levels (dBFS)
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AudioStats.h"
#include "Config.h"
#include "IHardwareDriver.h" // Hardware abstraction layer
#include "MPXMixer.h"
#include "NCO.h"
#include "NotchFilter19k.h"
#include "PolyphaseFIRUpsampler.h"
#include "PreemphasisFilter.h"
#include "RDSSynth.h"
#include "StereoMatrix.h"

// ==================================================================================
//                          AUDIO ENGINE CLASS
// ==================================================================================

/**
 * DSP_pipeline - FM Stereo Encoding Pipeline Orchestrator
 *
 * This class manages the complete audio processing pipeline from 48 kHz ADC input
 * to 192 kHz DAC output, including pre-emphasis, upsampling, and FM multiplex
 * synthesis. It runs as a high-priority FreeRTOS task on Core 0.
 *
 * Responsibilities:
 *   • I2S audio I/O (read from ADC, write to DAC)
 *   • DSP pipeline orchestration (7 processing stages)
 *   • Real-time performance monitoring
 *   • VU meter data generation
 *   • Diagnostic logging (optional, disabled by default)
 *
 * Usage Pattern:
 *   1. Call DSP_pipeline::startTask() to spawn the audio task
 *   2. Task creates an DSP_pipeline instance and calls begin()
 *   3. Infinite loop calls process() at 48 kHz block rate (every 1.33 ms)
 *   4. Each process() call reads one block, processes it, and writes output
 *
 * Thread Safety:
 *   DSP_pipeline is NOT thread-safe. Each instance runs on a single dedicated task.
 *   Multiple instances are not supported (singleton pattern enforced by startTask()).
 *
 * Performance:
 *   Typical processing time: ~300 µs per block (64 samples)
 *   Available time: 1333 µs (64 samples ÷ 48000 samples/sec)
 *   CPU usage: ~22% (300 µs ÷ 1333 µs)
 *   Headroom: ~78% (margin for system jitter and interrupts)
 *
 * Queue Overflow Behavior:
 *   VU meter samples and statistics snapshots are sent via mailbox (overwrite) queues.
 *   When the display task cannot consume samples fast enough:
 *   • Older samples are discarded and overwritten with newer ones
 *   • Consumer (VU display task) always sees the most recent audio levels
 *   • Prevents display lag and maintains responsive UI
 *   • Tradeoff: May miss transient peak captures if display can't keep up
 *
 * Communication Patterns (All via FreeRTOS Queues):
 *   • SENDS TO: VUMeter (audio levels, statistics)
 *   • SENDS TO: Logger (performance metrics, diagnostics)
 *   • READS FROM: RDSAssembler (RDS bit stream for injection)
 *   • READS FROM: Hardware driver (audio samples)
 *   Queue operations are non-blocking with overwrite semantics
 */
class DSP_pipeline
{
  public:
    // ==================================================================================
    //                          PUBLIC INTERFACE
    // ==================================================================================

    /**
     * Constructor
     *
     * Creates an DSP_pipeline instance with uninitialized DSP modules.
     * Hardware I2S interfaces are not configured until begin() is called.
     *
     * Parameters:
     *   hardware_driver:  Injected dependency for hardware I/O operations
     *                     (typically ESP32I2SDriver instance)
     *
     * Note: Buffers are zero-initialized by default (BSS section).
     *       hardware_driver must remain valid for the lifetime of DSP_pipeline.
     */
    explicit DSP_pipeline(IHardwareDriver *hardware_driver);

    /**
     * Initialize Audio Engine
     *
     * Called by the audio task after DSP_pipeline construction. Performs:
     *   1. I2S TX initialization (192 kHz DAC output)
     *   2. I2S RX initialization (48 kHz ADC input)
     *   3. NCO initialization (19 kHz pilot, 38 kHz subcarrier)
     *   4. Pre-emphasis filter initialization
     *   5. Upsampler initialization (loads polyphase FIR coefficients)
     *
     * Returns:
     *   true if all initialization succeeded
     *   false if I2S setup failed (check Serial for error messages)
     *
     * Note: This function must be called from the audio task context, not from
     *       the main setup() function. Use startTask() instead.
     */
    bool begin();

    /**
     * Process One Audio Block
     *
     * Main DSP processing function. Called in a tight loop by the audio task.
     * Processes exactly 64 stereo sample pairs (1.33 ms @ 48 kHz).
     *
     * Processing Steps:
     *   1. Read 64 samples from I2S RX (48 kHz, blocking)
     *   2. Convert int32 → float32 (Q31 to normalized [-1.0, +1.0])
     *   3. Apply pre-emphasis filter (50 µs time constant)
     *   4. Apply 19 kHz notch filter
     *   5. Upsample 48 kHz → 192 kHz (64 → 256 samples)
     *   6. Generate L+R (mono) and L-R (diff) signals
     *   7. Generate coherent 19/38/57 kHz carriers
     *   8. Mix mono + pilot + (diff × 38 kHz) + RDS = FM multiplex
     *   9. Convert float32 → int32 (normalized to Q31)
     *  10. Write 256 samples to I2S TX (192 kHz, blocking)
     *  11. Update VU meters (every 5 ms)
     *  12. Log performance stats (every 5 seconds)
     *
     * Performance Tracking:
     *   • Measures processing time using micros()
     *   • Calculates CPU usage percentage
     *   • Tracks peak audio levels for VU meters
     *
     * Blocking Behavior:
     *   This function blocks on I2S read/write operations. If I2S buffers are
     *   not ready, the task yields to FreeRTOS scheduler.
     *
     * Note: This function never returns. It's designed to be called in an
     *       infinite loop by the audio task.
     */
    void process();

    /**
     * Start Audio Task (Instance Method)
     *
     * Spawns a FreeRTOS task that runs the audio processing loop for this
     * specific DSP_pipeline instance. Used internally by startTask().
     *
     * Parameters:
     *   core_id:      FreeRTOS core to pin the task (must be 0 for audio)
     *   priority:     Task priority (typically 6, highest priority)
     *   stack_words:  Stack size in 32-bit words (typically 12288 = 48 KB)
     *
     * Returns:
     *   true if task creation succeeded
     *   false if task creation failed (out of memory or invalid parameters)
     *
     * Note: This is the instance-based method. Most users should call the
     *       static startTask() instead, which manages a singleton instance.
     */
    bool startTaskInstance(int core_id, UBaseType_t priority, uint32_t stack_words);

    /**
     * Start Audio Task (Static with Hardware Driver)
     *
     * Creates a DSP_pipeline instance with injected hardware driver and spawns
     * its task. This is the recommended way to start the audio engine with
     * explicit hardware dependency.
     *
     * Parameters:
     *   hardware_driver:  Hardware I/O implementation (e.g., ESP32I2SDriver)
     *   core_id:          FreeRTOS core (must be 0 for audio)
     *   priority:         Task priority (typically 6)
     *   stack_words:      Stack size in words (typically 12288)
     *
     * Returns:
     *   true if task creation succeeded
     *   false if task creation failed
     *
     * Example:
     *   ESP32I2SDriver driver;
     *   DSP_pipeline::startTask(&driver, 0, 6, 12288);
     *
     * Note: hardware_driver must remain valid for the lifetime of the audio task.
     */
    static bool startTask(IHardwareDriver *hardware_driver, int core_id, UBaseType_t priority,
                          uint32_t stack_words);

  private:
    // ==================================================================================
    //                          INTERNAL HELPER FUNCTIONS
    // ==================================================================================

    /**
     * Print Performance Statistics
     *
     * Called every 5 seconds by process() to log real-time performance metrics
     * to the Serial console via the logger queue.
     *
     * Parameters:
     *   frames_read:   Number of frames processed since last log
     *   available_us:  Available time per block (1333 µs @ 48 kHz)
     *   cpu_usage:     CPU usage percentage (0-100%)
     *   cpu_headroom:  Headroom percentage (0-100%)
     *
     * Output Format:
     *   [timestamp] DSP_pipeline: 48000 Hz, CPU 22.5%, Headroom 77.5%
     *   [timestamp] Peak: L=-12.3 dBFS, R=-14.1 dBFS
     */
    void printPerformance(std::size_t frames_read, float available_us, float cpu_usage,
                          float cpu_headroom);

    /**
     * Print Diagnostic Information
     *
     * Optional diagnostic logging for debugging DSP pipeline. Only compiled
     *   if DIAGNOSTIC_PRINT_INTERVAL > 0 in Config.h.
     *
     * Logs:
     *   • ADC input peak levels
     *   • Pre-emphasis output peak levels
     *   • FIR upsampler output peak levels
     *   • Pre-emphasis gain in dB
     *   • Total pipeline gain in dB
     *
     * Parameters:
     *   frames_read: Frame counter (used to throttle printing)
     *   peak_adc:    Peak ADC input (int32)
     *   peak_pre:    Peak pre-emphasis output (int32)
     *   peak_fir:    Peak FIR output (int32)
     *   pre_db:      Pre-emphasis gain (dB)
     *   total_db:    Total pipeline gain (dB)
     *
     * Note: This function is only available when DIAGNOSTIC_PRINT_INTERVAL > 0.
     */
    void printDiagnostics(std::size_t frames_read, int32_t peak_adc, int32_t peak_pre,
                          int32_t peak_fir, float pre_db, float total_db);

    // ==================================================================================
    //                      ORCHESTRATION HELPER METHODS
    // ==================================================================================

    /**
     * Read and convert audio from ADC, calculate VU levels
     */
    bool readAndConvertAudio(std::size_t &frames_read, float &l_peak, float &r_peak, float &l_rms,
                             float &r_rms, uint32_t &rx_wait_us, uint32_t cpu_mhz,
                             uint32_t &deint_us);

    /**
     * Update VU meter display with audio levels
     */
    void updateVUMeters(float l_peak, float r_peak, float l_rms, float r_rms,
                        std::size_t frames_read);

    /**
     * Convert float samples to int32 Q31 (computation only, no I/O)
     * Separated from I/O to properly measure conversion performance vs. I2S blocking time
     */
    void convertFloatToInt32(std::size_t frames_read);

    /**
     * Write samples to I2S DAC
     * Separated from conversion to properly measure I/O blocking time
     */
    void writeToDAC(std::size_t frames_read);

    /**
     * Update performance metrics and logs
     */
    void updatePerformanceMetrics(uint32_t total_us, std::size_t frames_read);

    // ==================================================================================
    //                          DSP PIPELINE MODULES
    // ==================================================================================

    // ---- FM Pre-Emphasis Filter ----
    // 50 µs time constant, boosts high frequencies for FM transmission
    PreemphasisFilter preemphasis_;

    // ---- 19 kHz Notch Filter ----
    // Removes pilot tone frequency content from audio to prevent interference
    NotchFilter19k notch_;

    // ---- Polyphase FIR Upsampler ----
    // 4× upsampling: 48 kHz → 192 kHz using 96-tap polyphase FIR (15 kHz LPF)
    PolyphaseFIRUpsampler upsampler_;

    // ---- Stereo Matrix ----
    // Generates L+R (mono) and L-R (stereo difference) signals
    StereoMatrix stereo_matrix_;

    // ---- FM Multiplex Mixer ----
    // Combines mono, pilot, and stereo subcarrier into FM MPX signal
    MPXMixer mpx_synth_;
    RDSSynth::Synth rds_synth_;

    // ---- Master NCO ----
    // pilot_19k_: master 19 kHz pilot; subcarrier (38 kHz) and RDS (57 kHz)
    // are derived coherently via harmonics from this phase.
    NCO pilot_19k_;

    // ---- Audio Statistics Tracker ----
    // Calculates peak, RMS, and dBFS levels for VU meter display
    AudioStats stats_;

    // ---- Hardware I/O Driver ----
    // Abstraction layer for I2S operations (injected dependency)
    // Enables testing and hardware independence
    IHardwareDriver *hardware_driver_;

    // ==================================================================================
    //                          AUDIO BUFFERS (16-BYTE ALIGNED)
    // ==================================================================================
    //
    // All buffers are aligned to 16-byte boundaries for potential SIMD optimization.
    // Buffer sizes are determined by Config::BLOCK_SIZE and UPSAMPLE_FACTOR.

    // ---- I2S Buffers (int32, Q31 fixed-point) ----
    alignas(16) int32_t rx_buffer_[Config::BLOCK_SIZE * 2];
    // RX buffer: 64 samples × 2 channels = 128 samples = 512 bytes
    // Holds 48 kHz stereo input from ADC (interleaved L, R, L, R, ...)

    alignas(16) int32_t tx_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR * 2];
    // TX buffer: 64 × 4 × 2 = 512 samples = 2048 bytes
    // Holds 192 kHz stereo output to DAC (interleaved L, R, L, R, ...)

    // ---- Floating-Point Processing Buffers ----
    alignas(16) float rx_f32_[Config::BLOCK_SIZE * 2];
    // RX float buffer: 64 × 2 = 128 floats = 512 bytes
    // Normalized float representation of ADC input [-1.0, +1.0]

    alignas(16) float tx_f32_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR * 2];
    // TX float buffer: 512 floats = 2048 bytes
    // Normalized float representation of DAC output [-1.0, +1.0]

    // ---- FM Synthesis Buffers ----
    alignas(16) float pilot_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    // Pilot tone buffer: 256 floats = 1024 bytes
    // 19 kHz sine wave for FM pilot tone

    alignas(16) float subcarrier_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    // Subcarrier buffer: 256 floats = 1024 bytes
    // 38 kHz sine wave for L-R modulation

    alignas(16) float mono_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    // Mono buffer: 256 floats = 1024 bytes
    // L+R signal (mono audio content, 0-15 kHz)

    alignas(16) float diff_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    // Diff buffer: 256 floats = 1024 bytes
    // L-R signal (stereo difference, modulated onto 38 kHz)

    alignas(16) float mpx_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    alignas(16) float carrier57_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    alignas(16) float rds_buffer_[Config::BLOCK_SIZE * Config::UPSAMPLE_FACTOR];
    // MPX buffer: 256 floats = 1024 bytes (plus RDS injection)
    // Final FM multiplex signal (mono + pilot + L-R subcarrier + RDS)

    // Total buffer memory: ~9 KB per DSP_pipeline instance

    // ==================================================================================
    //                          DIAGNOSTIC COUNTERS
    // ==================================================================================

#if DIAGNOSTIC_PRINT_INTERVAL > 0
    // Frame counter for throttling diagnostic output
    // Only compiled when diagnostics are enabled in Config.h
    uint32_t diagnostic_counter_ = 0;
#endif

    // ==================================================================================
    //                          FREERTOS TASK MANAGEMENT
    // ==================================================================================

    // Task handle for this DSP_pipeline instance
    // Used to suspend/resume/delete the task if needed
    TaskHandle_t task_handle_ = nullptr;

    // ==================================================================================
    //                          PILOT AUTO-MUTE STATE
    // ==================================================================================
    bool pilot_muted_ = false;                // Current mute state
    uint64_t last_above_thresh_us_ = 0;       // Last time audio was above threshold

    /**
     * Task Trampoline (Static Entry Point)
     *
     * FreeRTOS task entry point. Receives DSP_pipeline* as arg parameter.
     * Calls begin() to initialize, then enters infinite process() loop.
     *
     * Parameters:
     *   arg: Pointer to DSP_pipeline instance (void* for FreeRTOS compatibility)
     *
     * Execution Flow:
     *   1. Cast arg to DSP_pipeline*
     *   2. Call begin() to initialize I2S and DSP modules
     *   3. Enter infinite loop calling process()
     *   4. If begin() fails, task exits (should never happen in production)
     *
     * Note: This function never returns under normal operation.
     */
    static void taskTrampoline(void *arg);
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
