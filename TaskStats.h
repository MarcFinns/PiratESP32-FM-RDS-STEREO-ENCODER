// TaskStats.h
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
             float &logger_cpu,
             float &vu_cpu,
             uint32_t &audio_stack_free_words,
             uint32_t &logger_stack_free_words,
             uint32_t &vu_stack_free_words);
} // namespace TaskStats

