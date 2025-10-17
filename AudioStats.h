/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                         Audio Pipeline Performance Statistics
 *
 * =====================================================================================
 *
 * File:         AudioStats.h
 * Description:  Real-time performance profiling structures for DSP pipeline stages
 *
 * Purpose:
 *   This module defines data structures for monitoring and reporting the performance
 *   characteristics of the 8-stage audio processing pipeline (48 kHz input to 192 kHz
 *   output). It tracks per-stage timing (min/max/current), loop counters, and gain
 *   information for diagnostic and performance optimization purposes.
 *
 * Design Pattern: Data Structure Only
 *   AudioStats is a pure data container with in-place measurement methods. It does not
 *   perform I/O or allocate dynamic memory, making it safe for real-time use in
 *   interrupt contexts or on Core 0.
 *
 * Thread Safety:
 *   Not thread-safe. Should be accessed only from Core 0 audio processing task.
 *   Core 1 I/O tasks read-only via SystemContext (assumes snapshot atomicity).
 *
 * Performance Metrics:
 *   • stage_int_to_float: I2S RX → float conversion
 *   • stage_preemphasis: Pre-emphasis IIR filter (48 kHz)
 *   • stage_notch: 19 kHz notch filter (48 kHz)
 *   • stage_matrix: Stereo matrix L+R/L-R decomposition
 *   • stage_mpx: FM multiplex signal construction (pilot + subcarrier + RDS)
 *   • stage_upsample: 4× polyphase FIR upsampler (48 kHz → 192 kHz)
 *   • stage_float_to_int: float → I2S TX conversion
 *   • stage_rds: RDS bitstream injection and synthesis
 *
 * =====================================================================================
 */

#pragma once

#include <cstdint>

struct StageTiming
{
  uint32_t current = 0;
  uint32_t min = 0xFFFFFFFFu;
  uint32_t max = 0;

  void reset()
  {
    current = 0;
    min = 0xFFFFFFFFu;
    max = 0;
  }

  void update(uint32_t value)
  {
    current = value;
    if (value < min)
      min = value;
    if (value > max)
      max = value;
  }
};

struct AudioStats
{
  uint32_t loops_completed = 0;
  uint32_t errors = 0;
  uint64_t start_time_us = 0;
  uint64_t last_print_us = 0;

  StageTiming total;
  StageTiming stage_int_to_float;
  StageTiming stage_preemphasis;
  StageTiming stage_notch;
  StageTiming stage_matrix;
  StageTiming stage_mpx;
  StageTiming stage_upsample;
  StageTiming stage_float_to_int;
  StageTiming stage_rds; // RDS injection stage

  float gain_linear = 0.0f;
  float gain_db = 0.0f;
  bool gain_valid = false;

  void reset()
  {
    loops_completed = 0;
    errors = 0;
    start_time_us = 0;
    last_print_us = 0;
    total.reset();
    stage_int_to_float.reset();
    stage_preemphasis.reset();
    stage_notch.reset();
    stage_matrix.reset();
    stage_mpx.reset();
    stage_upsample.reset();
    stage_float_to_int.reset();
    stage_rds.reset();
    gain_linear = 0.0f;
    gain_db = 0.0f;
    gain_valid = false;
  }
};
