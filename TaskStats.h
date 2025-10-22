/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      FreeRTOS Task Performance Statistics
 *
 * =====================================================================================
 *
 * File:         TaskStats.h
 * Description:  Real-time task CPU utilization and stack usage monitoring
 *
 * Purpose:
 *   This module provides non-blocking access to FreeRTOS runtime statistics for
 *   monitoring task CPU load, stack watermarks, and core utilization on dual-core
 *   ESP32. It enables real-time diagnostics without halting audio processing.
 *
 * Monitored Tasks and Cores:
 *   • Core 0 (Real-Time Audio): Runs DSP_pipeline at 48 kHz → 192 kHz conversion
 *   • Core 1 (I/O Services): Runs Console and VUMeter tasks for display/error reporting
 *   • Per-task CPU%: Relative CPU time for named tasks (audio, console, vu)
 *   • Per-task stack: Free stack space (in 32-bit words for ESP32 FreeRTOS)
 *
 * Data Collection:
 *   collect() samples runtime statistics if FreeRTOS is configured with
 *   configGENERATE_RUN_TIME_STATS enabled. This requires:
 *     • A timer tick counter (usually from Timer 0 or similar)
 *     • Periodic sampling of task state (handled by FreeRTOS kernel)
 *
 *   Returns:
 *     • true: Statistics valid and collection succeeded
 *     • false: Runtime stats disabled or collection skipped (no data)
 *
 * CPU% Calculation:
 *   CPU% = (task_run_time / total_run_time) × 100
 *   On dual-core ESP32:
 *     • core0_load ≈ DSP_pipeline CPU% (should be 80–95% for normal streaming)
 *     • core1_load ≈ Console + VUMeter + other I/O (typically 10–30%)
 *
 * Stack Watermark:
 *   Free stack space (in 32-bit words):
 *     • If < 512 words (~2 KB): Warning, potential overflow risk
 *     • Normal: 1000–4000 words depending on task
 *
 * Thread Safety:
 *   Thread-safe for reading via non-blocking FreeRTOS API. Data is a snapshot;
 *   subsequent calls may return different values. init() must be called once
 *   during setup (idempotent).
 *
 * Limitations:
 *   • Requires FreeRTOS runtime stats enabled (configGENERATE_RUN_TIME_STATS)
 *   • CPU% accuracy depends on timer resolution
 *   • Not available on non-ESP32 platforms
 *
 * =====================================================================================
 */

#pragma once

#include <cstdint>

namespace TaskStats
{
// Initialize internal state for run-time sampling (no-op if not supported).
void init();

// Collect per-core load and per-task CPU%/stack watermark for named tasks.
// Returns true if CPU percentages are valid (requires run-time stats enabled).
bool collect(float &core0_load,
             float &core1_load,
             float &audio_cpu,
             float &logger_cpu,   // console task (serial/log)
             float &vu_cpu,
             uint32_t &audio_stack_free_words,
             uint32_t &logger_stack_free_words, // console task (serial/log)
             uint32_t &vu_stack_free_words);
} // namespace TaskStats
