/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                          VU Meter Display Interface
 *
 * =====================================================================================
 *
 * File:         VUMeter.h
 * Description:  Professional VU meter visualization API for ILI9341 TFT display
 *
 * Purpose:
 *   This module provides a clean interface for displaying real-time audio levels
 *   on an ILI9341 320×240 TFT display. It implements professional VU meter ballistics
 *   with color-coded level zones, peak hold markers, and smooth animation.
 *
 * Architecture:
 *   The VU meter runs as a separate FreeRTOS task on Core 1 (non-real-time core)
 *   to prevent display rendering from interfering with audio processing on Core 0.
 *   Audio samples are transferred via a single-slot FreeRTOS queue with overwrite
 *   semantics (mailbox pattern).
 *
 * Display Features:
 *   • Dual-channel stereo VU bars (left and right)
 *   • Color-coded zones: Green (safe), Yellow (moderate), Orange (high), Red (peak)
 *   • Peak hold markers with 1-second hold time
 *   • Linear dB scale from -40 dBFS to +3 dBFS
 *   • 50 FPS update rate for smooth animation
 *   • Delta rendering to minimize SPI traffic
 *
 * Ballistics:
 *   • Attack time: Fast (~10 ms) for immediate response to transients
 *   • Release time: Slow (~500 ms) for easier visual tracking
 *   • Peak markers: Snap to peaks instantly, decay after 1-second hold
 *
 * Communication Pattern:
 *   1. DSP_pipeline (Core 0) calculates peak/RMS levels every 1.33 ms
 *   2. Sends VUMeter::Sample via queue (overwrite if full)
 *   3. VU task (Core 1) reads latest sample every 20 ms (50 FPS)
 *   4. Applies ballistics and renders to ILI9341 display
 *
 * Thread Safety:
 *   All public functions are thread-safe and can be called from any task.
 *   The FreeRTOS queue provides atomic operations for sample transfer.
 *
 * =====================================================================================
 */

#pragma once

// VU Meter task and enqueue API.
//
// Runs on the logger core to avoid perturbing the audio pipeline.
// The audio task sends periodic peak/RMS summaries via a single-slot queue
// (mailbox) using overwrite semantics so the consumer always sees the latest.

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>

namespace VUMeter
{
    // ==================================================================================
    //                          VU METER SAMPLE STRUCTURE
    // ==================================================================================

    /**
     * VU Meter Sample Payload
     *
     * Compact data structure sent from DSP_pipeline to VU meter task.
     * Contains all audio level metrics for both channels.
     *
     * Size: 28 bytes (7 × 4 bytes)
     *
     * Fields:
     *   l_rms:   RMS (root mean square) level for left channel [0.0, 1.0]
     *   r_rms:   RMS level for right channel [0.0, 1.0]
     *   l_peak:  Peak (maximum absolute value) for left channel [0.0, 1.0]
     *   r_peak:  Peak for right channel [0.0, 1.0]
     *   l_dbfs:  Left channel level in dBFS (decibels relative to full scale)
     *   r_dbfs:  Right channel level in dBFS
     *   frames:  Number of audio frames summarized in this sample
     *   ts_us:   Timestamp in microseconds when sample was captured
     *
     * RMS vs. Peak:
     *   • RMS represents average power (perceived loudness)
     *   • Peak represents maximum instantaneous level (clipping potential)
     *   • The VU display can be configured to use either metric for bar length
     *
     * dBFS Scale:
     *   dBFS (decibels relative to full scale) is a logarithmic representation:
     *     0 dBFS = maximum possible level (digital full scale)
     *    -6 dBFS = half amplitude
     *   -20 dBFS = 10% amplitude
     *   -40 dBFS = 1% amplitude (typical noise floor)
     *   +3 dBFS = overload/clipping (red zone on VU meter)
     *
     * Usage:
     *   DSP_pipeline fills this structure every 1.33 ms and sends via enqueue().
     *   VU task reads the latest sample and applies ballistics for display.
     */
    struct Sample
    {
        float    l_rms;     // Left RMS level [0.0, 1.0]
        float    r_rms;     // Right RMS level [0.0, 1.0]
        float    l_peak;    // Left peak level [0.0, 1.0]
        float    r_peak;    // Right peak level [0.0, 1.0]
        float    l_dbfs;    // Left dBFS [-∞, +3 dB]
        float    r_dbfs;    // Right dBFS [-∞, +3 dB]
        uint32_t frames;    // Number of frames in this summary
        uint32_t ts_us;     // Timestamp (microseconds from micros())
    };

    // ==================================================================================
    //                          VU METER TASK MANAGEMENT
    // ==================================================================================

    /**
     * Start VU Meter Task
     *
     * Creates a FreeRTOS task that continuously reads audio samples from a queue
     * and renders VU meters on the ILI9341 display. The task runs at ~50 FPS.
     *
     * Task Responsibilities:
     *   • Read VU samples from queue (blocking, 20 ms timeout)
     *   • Apply attack/release ballistics for smooth animation
     *   • Update peak hold markers with 1-second hold time
     *   • Render color-coded VU bars with delta optimization
     *   • Handle display initialization and error recovery
     *
     * Parameters:
     *   core_id:      FreeRTOS core to pin the task (0 or 1, typically 1)
     *                 Core 1 is recommended to keep display rendering off the
     *                 audio core (Core 0).
     *
     *   priority:     Task priority (typically 1, lower than logger and audio)
     *                 VU updates are low priority since they're visual feedback,
     *                 not critical for audio processing.
     *
     *   stack_words:  Stack size in 32-bit words (typically 4096 = 16 KB)
     *                 Sufficient for Arduino_GFX library calls and local buffers.
     *
     *   queue_len:    Queue depth in samples (default: 1 for mailbox behavior)
     *                 A queue length of 1 implements overwrite semantics:
     *                 the producer always overwrites the current value, ensuring
     *                 the consumer sees the latest sample (no stale data).
     *
     * Returns:
     *   true if task and queue creation succeeded
     *   false if initialization failed (likely out of memory)
     *
     * Example:
     *   VUMeter::startTask(1, 1, 4096, 1);  // Core 1, priority 1, mailbox queue
     *
     * Note: If Config::VU_DISPLAY_ENABLED is false, this function may
     *       return true but the display will not initialize.
     */
    bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words,
                   std::size_t queue_len = 1);

    /**
     * Stop VU Meter Task
     *
     * Terminates the VU meter task and deletes its queue, freeing resources.
     * Display is left in its current state (not cleared).
     *
     * Use Cases:
     *   • Shutting down before deep sleep
     *   • Reconfiguring display parameters
     *   • Releasing resources for other tasks
     *
     * Note: After calling stopTask(), startTask() must be called again to
     *       resume VU meter operation.
     */
    void stopTask();

    // ==================================================================================
    //                          SAMPLE ENQUEUE FUNCTIONS
    // ==================================================================================

    /**
     * Enqueue VU Sample (Task Context)
     *
     * Sends a VU meter sample to the display task. This function is called by
     * DSP_pipeline every 1.33 ms with updated audio level metrics.
     *
     * Mailbox Behavior:
     *   If queue_len = 1 (default), this function implements mailbox semantics:
     *     • If queue is empty, sample is enqueued normally
     *     • If queue is full (already has 1 sample), the old sample is overwritten
     *     • Consumer always sees the most recent sample (no stale data)
     *
     * Parameters:
     *   s: VU meter sample containing peak, RMS, dBFS for both channels
     *
     * Returns:
     *   true if sample was successfully enqueued or overwrote existing sample
     *   false if queue was not initialized (startTask() not called)
     *
     * Performance:
     *   • Enqueue time: ~5-10 µs (FreeRTOS queue overhead)
     *   • Non-blocking: Returns immediately even if queue is full
     *
     * Thread Safety:
     *   Safe to call from any task. Uses FreeRTOS atomic queue operations.
     *
     * Example:
     *   VUMeter::Sample s;
     *   s.l_peak = left_peak;
     *   s.r_peak = right_peak;
     *   s.l_dbfs = 20.0f * log10f(left_peak);
     *   // ... fill remaining fields
     *   VUMeter::enqueue(s);
     */
    bool enqueue(const Sample& s);

    /**
     * Enqueue VU Sample (ISR Context)
     *
     * ISR-safe variant of enqueue() for use in interrupt service routines.
     * Currently not used in this project (audio processing runs in tasks, not ISRs).
     *
     * Parameters:
     *   s:                            VU meter sample to enqueue
     *   pxHigherPriorityTaskWoken:    Pointer to FreeRTOS flag indicating if
     *                                 a higher-priority task was woken by this call
     *                                 (used for context switching after ISR)
     *
     * Returns:
     *   true if sample was successfully enqueued
     *   false if queue was not initialized
     *
     * Note: This function uses xQueueOverwriteFromISR() for atomic operation
     *       from interrupt context. After calling this function, the caller
     *       must check *pxHigherPriorityTaskWoken and yield if necessary.
     *
     * Example (ISR usage):
     *   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
     *   VUMeter::enqueueFromISR(sample, &xHigherPriorityTaskWoken);
     *   portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
     */
    bool enqueueFromISR(const Sample& s, BaseType_t* pxHigherPriorityTaskWoken);

    // ==================================================================================
    //                             STATUS PANEL SNAPSHOT
    // ==================================================================================
    struct StatsSnapshot
    {
        float    cpu_usage;    // percent
        float    cpu_headroom; // percent
        float    total_us_cur; // processing time current
        float    total_us_min; // min
        float    total_us_max; // max
        float    fir_us_cur;   // upsample stage current
        float    mpx_us_cur;   // mpx stage current
        float    matrix_us_cur; // stereo matrix stage current
        float    rds_us_cur;    // RDS injection stage current
        uint32_t heap_free;    // bytes
        uint32_t heap_min;     // bytes
        uint32_t uptime_s;     // seconds
        // Optional extended fields (if run-time stats available)
        float    core0_load;   // percent (approx)
        float    core1_load;   // percent (approx)
        float    audio_cpu;    // percent of total
        float    logger_cpu;   // percent of total
        float    vu_cpu;       // percent of total
        uint32_t audio_stack_free_words; // words
        uint32_t logger_stack_free_words; // words
        uint32_t vu_stack_free_words;     // words
        uint8_t  cpu_valid;    // 1 if CPU percentages valid
        // Core loop stats
        uint32_t loops_completed;
        uint32_t errors;
    };

    // Enqueue a status snapshot for bottom panel (single-slot queue, overwrite)
    bool enqueueStats(const StatsSnapshot& s);

} // namespace VUMeter

// =====================================================================================
//                                END OF FILE
// =====================================================================================
