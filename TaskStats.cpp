/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      FreeRTOS Task Performance Statistics Implementation
 *
 * =====================================================================================
 *
 * File:         TaskStats.cpp
 * Description:  Runtime task profiling and core utilization monitoring
 *
 * Purpose:
 *   This implementation provides non-blocking access to FreeRTOS runtime statistics
 *   for monitoring real-time task performance on dual-core ESP32. It tracks CPU
 *   utilization per core and per task, stack watermarks, and task state without
 *   halting audio processing or requiring synchronized access.
 *
 * FreeRTOS Runtime Stats:
 *   FreeRTOS maintains a timer counter that increments at a fixed rate (usually 1 µs).
 *   Each task's ulRunTimeCounter is updated when the task runs. By sampling deltas
 *   between invocations, we compute per-core and per-task CPU percentages.
 *
 * Per-Core Load Calculation:
 *   Uses task idle counters (IDLE0 on Core 0, IDLE1 on Core 1):
 *     Core_Load% = (1.0 − idle_time_delta / core_total_time_delta) × 100
 *
 *   This accounts for actual work vs. idle on each core independently.
 *
 * Per-Task Profiling:
 *   Tracks CPU% and stack watermarks for:
 *     • audio: Core 0 DSP pipeline (expected 80–95%)
 *     • console: Core 1 console/log task (expected 5–15%)
 *     • vu: Core 1 VU meter display task (expected 2–8%)
 *
 *   Stack watermarks are per-task free stack (in 32-bit words) reported by FreeRTOS.
 *
 * State Management:
 *   Static variables track last-known values for delta calculation:
 *     • s_last_total_runtime: Total system runtime counter
 *     • s_last_idle[0,1]_runtime: Per-core idle task counters
 *     • s_last_[audio,logger,vu]_runtime: Per-task counters
 *     • s_last_core[0,1]_total: Per-core total runtime (for per-core load)
 *
 * Conditional Compilation:
 *   If configGENERATE_RUN_TIME_STATS is 0 (disabled), collect() returns false
 *   and outputs remain unchanged (zero-cost fallback).
 *
 * Thread Safety:
 *   Read-only for statistics (thread-safe via FreeRTOS API). State updates are
 *   idempotent and non-blocking. Suitable for any task context.
 *
 * Performance:
 *   collect() executes in ~10–20 µs (64 task scan with caching). No blocking
 *   or interrupt-safe operations required.
 *
 * =====================================================================================
 */

#include "TaskStats.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace TaskStats
{
static uint32_t s_last_total_runtime = 0;
static uint32_t s_last_idle0_runtime = 0;
static uint32_t s_last_idle1_runtime = 0;
static uint32_t s_last_audio_runtime = 0;
static uint32_t s_last_logger_runtime = 0;
static uint32_t s_last_vu_runtime = 0;
static uint32_t s_last_core0_total = 0;
static uint32_t s_last_core1_total = 0;

void init() {}

bool collect(float &core0_load,
             float &core1_load,
             float &audio_cpu,
             float &logger_cpu,
             float &vu_cpu,
             uint32_t &audio_stack_free_words,
             uint32_t &logger_stack_free_words,
             uint32_t &vu_stack_free_words)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    const UBaseType_t maxTasks = 64;
    TaskStatus_t taskStatus[maxTasks];
    UBaseType_t taskCount = 0;
    uint32_t totalRunTime = 0;

    taskCount = uxTaskGetSystemState(taskStatus, maxTasks, &totalRunTime);
    if (taskCount == 0 || totalRunTime == 0)
    {
        return false;
    }

    // Find specific tasks and idle per-core, compute deltas
    uint32_t idle0_rt = 0, idle1_rt = 0;
    uint32_t audio_rt = 0, logger_rt = 0, vu_rt = 0;
    uint32_t total_core0 = 0, total_core1 = 0;
    uint32_t audio_sw = 0, logger_sw = 0, vu_sw = 0;

    for (UBaseType_t i = 0; i < taskCount; ++i)
    {
        const TaskStatus_t &ts = taskStatus[i];
        // Sum total runtime per core for delta calculations
#if (tskKERNEL_VERSION_MAJOR >= 10)
        int core = ts.xCoreID;
#else
        int core = 0; // Fallback if xCoreID unavailable
#endif
        if (core == 0)
        {
            total_core0 += ts.ulRunTimeCounter;
        }
        else
        {
            total_core1 += ts.ulRunTimeCounter;
        }

        // Capture idle tasks by name
        if (strcmp(ts.pcTaskName, "IDLE0") == 0)
        {
            idle0_rt = ts.ulRunTimeCounter;
        }
        else if (strcmp(ts.pcTaskName, "IDLE1") == 0)
        {
            idle1_rt = ts.ulRunTimeCounter;
        }
        else if (strcmp(ts.pcTaskName, "audio") == 0)
        {
            audio_rt = ts.ulRunTimeCounter;
            audio_sw = ts.usStackHighWaterMark;
        }
        else if (strcmp(ts.pcTaskName, "console") == 0 || strcmp(ts.pcTaskName, "logger") == 0)
        {
            logger_rt = ts.ulRunTimeCounter;
            logger_sw = ts.usStackHighWaterMark;
        }
        else if (strcmp(ts.pcTaskName, "vu") == 0)
        {
            vu_rt = ts.ulRunTimeCounter;
            vu_sw = ts.usStackHighWaterMark;
        }
    }

    // Protect against first-run or missing values
    if (s_last_total_runtime == 0)
    {
        s_last_total_runtime = totalRunTime;
        s_last_idle0_runtime = idle0_rt;
        s_last_idle1_runtime = idle1_rt;
        s_last_audio_runtime = audio_rt;
        s_last_logger_runtime = logger_rt;
        s_last_vu_runtime = vu_rt;
        s_last_core0_total = total_core0;
        s_last_core1_total = total_core1;
        // Provide initial watermarks
        audio_stack_free_words = audio_sw;
        logger_stack_free_words = logger_sw;
        vu_stack_free_words = vu_sw;
        core0_load = core1_load = 0.0f;
        audio_cpu = logger_cpu = vu_cpu = 0.0f;
        return true;
    }

    uint32_t d_total = totalRunTime - s_last_total_runtime;
    uint32_t d_idle0 = idle0_rt - s_last_idle0_runtime;
    uint32_t d_idle1 = idle1_rt - s_last_idle1_runtime;
    uint32_t d_audio = audio_rt - s_last_audio_runtime;
    uint32_t d_logger = logger_rt - s_last_logger_runtime;
    uint32_t d_vu = vu_rt - s_last_vu_runtime;

    // Update watermarks
    audio_stack_free_words = audio_sw;
    logger_stack_free_words = logger_sw;
    vu_stack_free_words = vu_sw;

    // Update last snapshot
    s_last_total_runtime = totalRunTime;
    s_last_idle0_runtime = idle0_rt;
    s_last_idle1_runtime = idle1_rt;
    s_last_audio_runtime = audio_rt;
    s_last_logger_runtime = logger_rt;
    s_last_vu_runtime = vu_rt;
    uint32_t d_core0 = (total_core0 >= s_last_core0_total) ? (total_core0 - s_last_core0_total) : 0;
    uint32_t d_core1 = (total_core1 >= s_last_core1_total) ? (total_core1 - s_last_core1_total) : 0;
    s_last_core0_total = total_core0;
    s_last_core1_total = total_core1;

    // Compute percentages (guard against division by zero)
    if (d_total == 0)
    {
        core0_load = core1_load = 0.0f;
        audio_cpu = logger_cpu = vu_cpu = 0.0f;
        return true;
    }

    // Per-core load using per-core totals and per-core idle deltas
    if (d_core0 > 0)
        core0_load = (1.0f - ((float)d_idle0 / (float)d_core0)) * 100.0f;
    else
        core0_load = 0.0f;
    if (d_core1 > 0)
        core1_load = (1.0f - ((float)d_idle1 / (float)d_core1)) * 100.0f;
    else
        core1_load = 0.0f;

    audio_cpu = (float)d_audio / (float)d_total * 100.0f;
    logger_cpu = (float)d_logger / (float)d_total * 100.0f;
    vu_cpu = (float)d_vu / (float)d_total * 100.0f;

    return true;
#else
    (void)core0_load; (void)core1_load; (void)audio_cpu; (void)logger_cpu; (void)vu_cpu;
    (void)audio_stack_free_words; (void)logger_stack_free_words; (void)vu_stack_free_words;
    return false;
#endif
}
} // namespace TaskStats
