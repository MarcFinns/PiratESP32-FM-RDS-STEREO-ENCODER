/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       Display Manager (ModuleBase)
 *
 * =====================================================================================
 *
 * File:         DisplayManager.h
 * Description:  Display manager module handling VU visualization and UI text
 *
 * Purpose:
 *   This module provides a clean interface for displaying real-time audio levels
 *   on an ILI9341 320x240 TFT display. It implements professional VU meter ballistics
 *   with color-coded level zones, peak hold markers, and smooth animation.
 *
 * Architecture:
 *   The VU meter runs as a separate FreeRTOS task on Core 1 (non-real-time core)
 *   to prevent display rendering from interfering with audio processing on Core 0.
 *   Audio samples are transferred via a single-slot FreeRTOS queue with overwrite
 *   semantics (mailbox pattern).
 *
 * Display Features:
 *   * Dual-channel stereo VU bars (left and right)
 *   * Color-coded zones: Green (safe), Yellow (moderate), Orange (high), Red (peak)
 *   * Peak hold markers with 1-second hold time
 *   * Linear dB scale from -40 dBFS to +3 dBFS
 *   * 50 FPS update rate for smooth animation
 *   * Delta rendering to minimize SPI traffic
 *
 * Ballistics:
 *   * Attack time: Fast (~10 ms) for immediate response to transients
 *   * Release time: Slow (~500 ms) for easier visual tracking
 *   * Peak markers: Snap to peaks instantly, decay after 1-second hold
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
 *   enqueue() and enqueueStats() are non-blocking and never wait.
 *
 * Queue Overflow Behavior:
 *   Sample Queue (queue_): Uses mailbox (overwrite) semantics with queue_len=1.
 *   • If queue empty: New sample enqueued normally
 *   • If queue full: Old sample overwritten with new one
 *   • Consumer always sees latest audio level (no stale data)
 *   • Prevents display lag but may miss transient peaks
 *   • Overflow count incremented when overwrite occurs
 *
 *   Stats Queue (stats_queue_): Similar mailbox pattern for system statistics.
 *   • Statistics updated every 1 second, displayed at ~50 FPS
 *   • Single slot ensures responsive UI with minimal memory usage
 *
 * =====================================================================================
 */

#pragma once

#include "ErrorHandler.h"
#include "ModuleBase.h"

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Forward declarations to avoid including heavy GFX headers here
class Arduino_DataBus;
class Arduino_GFX;

/**
 * VU Meter Sample Payload
 *
 * Compact data structure sent from DSP_pipeline to VU meter task.
 * Contains all audio level metrics for both channels.
 *
 * Size: 28 bytes (7 x 4 bytes)
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
 *   * RMS represents average power (perceived loudness)
 *   * Peak represents maximum instantaneous level (clipping potential)
 *   * The VU display can be configured to use either metric for bar length
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
struct VUSample
{
    float l_rms;     // Left RMS level [0.0, 1.0]
    float r_rms;     // Right RMS level [0.0, 1.0]
    float l_peak;    // Left peak level [0.0, 1.0]
    float r_peak;    // Right peak level [0.0, 1.0]
    float l_dbfs;    // Left dBFS [-inf, +3 dB]
    float r_dbfs;    // Right dBFS [-inf, +3 dB]
    uint32_t frames; // Number of frames in this summary
    uint32_t ts_us;  // Timestamp (microseconds from micros())
};

/**
 * VU Meter Status Snapshot
 *
 * Extended statistics snapshot for bottom status panel display.
 */
struct VUStatsSnapshot
{
    float cpu_usage;     // percent
    float cpu_headroom;  // percent
    float total_us_cur;  // processing time current
    float total_us_min;  // min
    float total_us_max;   // max
    float fir_us_cur;    // upsample stage current
    float mpx_us_cur;    // mpx stage current
    float matrix_us_cur; // stereo matrix stage current
    float rds_us_cur;    // RDS injection stage current
    uint32_t heap_free;  // bytes
    uint32_t heap_min;   // bytes
    uint32_t uptime_s;   // seconds
    // Optional extended fields (if run-time stats available)
    float core0_load;                 // percent (approx)
    float core1_load;                 // percent (approx)
    float audio_cpu;                  // percent of total
    float logger_cpu;                 // percent of total
    float vu_cpu;                     // percent of total
    uint32_t audio_stack_free_words;  // words
    uint32_t logger_stack_free_words; // words
    uint32_t vu_stack_free_words;     // words
    uint8_t cpu_valid;                // 1 if CPU percentages valid
    // Core loop stats
    uint32_t loops_completed;
    uint32_t errors;
};

/**
 * VUMeter - Professional VU Meter Display Module
 *
 * Inherits from ModuleBase to provide unified FreeRTOS task lifecycle management.
 * Displays real-time audio levels on ILI9341 TFT display with professional ballistics.
 *
 * Queue Semantics (See QueueContracts.md for full design rationale):
 *
 *   Sample Queue:
 *     * Type: Mailbox (single-slot queue with overwrite semantics)
 *     * Size: 1 VUSample per queue (28 bytes)
 *     * Behavior: Non-blocking xQueueOverwrite() - always succeeds
 *       - If queue full (already has 1 sample): overwrites old sample
 *       - If queue empty: enqueues new sample
 *     * Rationale: Display only needs latest peak levels. Audio levels change
 *       every 5ms but display updates at 50 FPS (20ms). Older samples are
 *       irrelevant; only current level matters.
 *     * Tradeoff: Minimal memory (28 bytes), always non-blocking, but cannot
 *       track level history or catch rapid transients.
 *
 *   Stats Queue:
 *     * Type: Mailbox (single-slot queue with overwrite semantics)
 *     * Size: 1 VUStatsSnapshot per queue (~140 bytes)
 *     * Behavior: Non-blocking xQueueOverwrite() - always succeeds
 *       - Updates every 1 second from DSP_pipeline
 *       - Display reads at ~50 FPS (20ms interval)
 *     * Rationale: CPU/heap/stack metrics valid for ~1 second. Older snapshots
 *       immediately stale. Single slot keeps display responsive.
 *     * Tradeoff: Cannot observe state transitions or detect transient spikes.
 *
 * Thread Safety:
 *   All public functions are safe to call from any task.
 *   FreeRTOS queue provides atomic enqueue operations.
 *   Producer (DSP_pipeline on Core 0) and Consumer (VUMeter on Core 1)
 *   never block each other.
 *
 * Backward Compatibility:
 *   Static wrapper methods maintain compatibility with original namespace-based API.
 */
class DisplayManager : public ModuleBase
{
  public:
    /**
     * Get VUMeter Singleton Instance
     *
     * Returns reference to the single global VUMeter instance.
     *
     * Returns:
     *   Reference to the singleton VUMeter instance
     */
    static DisplayManager &getInstance();

    /**
     * Start VU Meter Task
     *
     * Creates a FreeRTOS task that continuously reads audio samples from a queue
     * and renders VU meters on the ILI9341 display. The task runs at ~50 FPS.
     *
     * Task Responsibilities:
     *   * Read VU samples from queue (blocking, 20 ms timeout)
     *   * Apply attack/release ballistics for smooth animation
     *   * Update peak hold markers with 1-second hold time
     *   * Render color-coded VU bars with delta optimization
     *   * Handle display initialization and error recovery
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
     *   VUMeter::startTask(1, 1, 4096, 1);
     *
     * Note: If Config::VU_DISPLAY_ENABLED is false, this function may
     *       return true but the display will not initialize.
     */
    static bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words,
                          size_t queue_len = 1);

    /**
     * Stop VU Meter Task
     *
     * Terminates the VU meter task and deletes its queue, freeing resources.
     * Display is left in its current state (not cleared).
     *
     * Use Cases:
     *   * Shutting down before deep sleep
     *   * Reconfiguring display parameters
     *   * Releasing resources for other tasks
     *
     * Note: After calling stopTask(), startTask() must be called again to
     *       resume VU meter operation.
     */
    static void stopTask();

    /**
     * Returns true once the display task has successfully initialized
     * and entered its process() loop.
     */
    static bool isReady();

    /**
     * Enqueue VU Sample (Task Context)
     *
     * Sends a VU meter sample to the display task. This function is called by
     * DSP_pipeline every 1.33 ms with updated audio level metrics.
     *
     * Mailbox Behavior:
     *   If queue_len = 1 (default), this function implements mailbox semantics:
     *     * If queue is empty, sample is enqueued normally
     *     * If queue is full (already has 1 sample), the old sample is overwritten
     *     * Consumer always sees the most recent sample (no stale data)
     *
     * Parameters:
     *   s: VU meter sample containing peak, RMS, dBFS for both channels
     *
     * Returns:
     *   true if sample was successfully enqueued or overwrote existing sample
     *   false if queue was not initialized (startTask() not called)
     *
     * Performance:
     *   * Enqueue time: ~5-10 us (FreeRTOS queue overhead)
     *   * Non-blocking: Returns immediately even if queue is full
     *
     * Thread Safety:
     *   Safe to call from any task. Uses FreeRTOS atomic queue operations.
     *
     * Example:
     *   VUSample s;
     *   s.l_peak = left_peak;
     *   s.r_peak = right_peak;
     *   s.l_dbfs = 20.0f * log10f(left_peak);
     *   VUMeter::enqueue(s);
     */
    static bool enqueue(const VUSample &s);

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
     *
     * Returns:
     *   true if sample was successfully enqueued
     *   false if queue was not initialized
     *
     * Note: Uses xQueueOverwriteFromISR() for atomic operation from interrupt context.
     *       After calling, check *pxHigherPriorityTaskWoken and yield if necessary.
     */
    static bool enqueueFromISR(const VUSample &s, BaseType_t *pxHigherPriorityTaskWoken);

    /**
     * Enqueue Statistics Snapshot
     *
     * Sends extended statistics snapshot for bottom status panel display.
     *
     * Parameters:
     *   s: Statistics snapshot containing CPU, heap, timing, and task stats
     *
     * Returns:
     *   true if snapshot was successfully enqueued
     *   false if queue was not initialized
     */
    static bool enqueueStats(const VUStatsSnapshot &s);

    /**
     * Set long-form RadioText for display marquee (UI only)
     *
     * The display can show an arbitrarily long concatenation of RT pieces
     * (e.g., multiple lines joined with a delimiter). This UI string is not
     * constrained by the 64-char RDS limit and is independent of what is
     * currently being broadcast.
     *
     * Parameters:
     *   rt_long: UTF-8 string to scroll on the display; nullptr clears it
     */
    static void setDisplayRT(const char *rt_long);

  private:
    /**
     * Private Constructor (Singleton Pattern)
     *
     * Initializes module state. Only called by getInstance().
     */
    DisplayManager();

    /**
     * Virtual Destructor Implementation
     */
    virtual ~DisplayManager() = default;

    /**
     * Initialize Module Resources (ModuleBase contract)
     *
     * Called once when the task starts. Initializes display and prepares
     * the VU meter for operation.
     *
     * Returns:
     *   true if initialization successful, false otherwise
     */
    bool begin() override;

    /**
     * Main Processing Loop Body (ModuleBase contract)
     *
     * Called repeatedly in infinite loop. Reads samples from queue,
     * applies ballistics, and renders to display at ~50 FPS.
     */
    void process() override;

    /**
     * Shutdown Module Resources (ModuleBase contract)
     *
     * Called during graceful shutdown. Cleans up display resources.
     */
    void shutdown() override;

    /**
     * Task Trampoline (FreeRTOS Entry Point)
     *
     * Static function called by FreeRTOS when task starts.
     * Delegates to ModuleBase::defaultTaskTrampoline().
     *
     * Parameters:
     *   arg: Pointer to VUMeter instance
     */
    static void taskTrampoline(void *arg);

    /**
     * Instance Method - Enqueue VU Sample
     *
     * Core implementation using FreeRTOS mailbox overwrite semantics.
     */
    bool enqueueRaw(const VUSample &s);

    /**
     * Instance Method - Enqueue VU Sample from ISR
     *
     * ISR-safe variant of enqueueRaw().
     */
    bool enqueueFromISRRaw(const VUSample &s, BaseType_t *pxHigherPriorityTaskWoken);

    /**
     * Instance Method - Enqueue Statistics Snapshot
     */
    bool enqueueStatsRaw(const VUStatsSnapshot &s);

    // Member State Variables
    QueueHandle_t queue_;       ///< FreeRTOS queue for VU samples
    QueueHandle_t stats_queue_; ///< FreeRTOS queue for stats snapshots
    size_t queue_len_;          ///< Queue depth in samples
    int core_id_;               ///< FreeRTOS core ID
    UBaseType_t priority_;      ///< Task priority
    uint32_t stack_words_;      ///< Stack size in words

    // Display resources (initialized in begin())
    Arduino_DataBus *bus_ = nullptr;
    Arduino_GFX *gfx_ = nullptr;

    // Error Tracking
    volatile uint32_t sample_overflow_count_; ///< Count of VU sample queue overflows
    volatile uint32_t stats_overflow_count_;  ///< Count of stats queue overflows
    volatile bool sample_overflow_logged_;    ///< First overflow logged flag (prevent spam)
};

// Backward compatibility aliases for old namespace-based API
// Backward compatibility aliases
//using VUMeter = DisplayManager;

namespace DisplayManager_NS
{
// Legacy type names - map to new structs
using Sample = VUSample;
using StatsSnapshot = VUStatsSnapshot;

// Delegate to class static methods
inline bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words, size_t queue_len = 1)
{
    return DisplayManager::startTask(core_id, priority, stack_words, queue_len);
}

inline void stopTask()
{
    DisplayManager::stopTask();
}

inline bool enqueue(const Sample &s)
{
    return DisplayManager::enqueue(s);
}

inline bool enqueueFromISR(const Sample &s, BaseType_t *pxHigherPriorityTaskWoken)
{
    return DisplayManager::enqueueFromISR(s, pxHigherPriorityTaskWoken);
}

inline bool enqueueStats(const StatsSnapshot &s)
{
    return DisplayManager::enqueueStats(s);
}
} // namespace DisplayManager_NS

// =====================================================================================
//                                END OF FILE
// =====================================================================================
