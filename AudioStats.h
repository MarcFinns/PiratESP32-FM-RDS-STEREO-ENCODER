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
