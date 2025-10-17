/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                    FM Pre-Emphasis Filter (IIR First-Order)
 *
 * =====================================================================================
 *
 * File:         PreemphasisFilter.h
 * Description:  FM broadcast pre-emphasis (high-frequency boost) for noise reduction
 *
 * Purpose:
 *   This module implements the standard FM broadcast pre-emphasis filter, a first-order
 *   IIR filter that boosts high frequencies (treble) before transmission. FM receivers
 *   apply a complementary de-emphasis filter to restore flat response and reduce
 *   high-frequency noise (HF noise is typically more prevalent in the RF channel).
 *
 *   Pre-emphasis is standardized across FM broadcast systems:
 *     • 75 μs time constant (North America, most of world) → cutoff ~2.1 kHz
 *     • 50 μs time constant (Europe) → cutoff ~3.2 kHz
 *
 *   Discrete-time implementation used here (allocation-free, stable):
 *     y[n] = gain · (x[n] − α · x[n−1])
 *     where α = exp(−1 / (τ · fs)), τ in seconds, fs = 48 kHz (input domain)
 *
 *   Notes:
 *     • This “leaky differentiator” form implements the pre‑emphasis high‑pass
 *       response (zero at DC, +20 dB/dec slope) and is standard for simple FM encoders.
 *     • Use PREEMPHASIS_GAIN to manage overall program level after treble boost.
 *
 * Stereo Processing:
 *   Left and right channels are filtered independently with separate state variables
 *   (prev_left_, prev_right_) to preserve stereo image and prevent crosstalk.
 *
 * Design:
 *   • First-order high‑pass (1 zero at z=1) with leaky term α
 *   • Difference form: y[n] = gain · (x[n] − α · x[n−1])
 *   • In-place processing (output buffer can be same as input)
 *   • Configurable via configure(alpha, gain) for flexibility
 *
 * Thread Safety:
 *   Not thread-safe. Must be called exclusively from Core 0 audio processing task
 *   at 48 kHz block rate. reset() must not be called while process() is active.
 *
 * Typical Configuration:
 *   configure(0.015, 1.0) for 75 μs pre-emphasis with unity DC gain
 *   (alpha value depends on sample rate; 0.015 is appropriate for 48 kHz)
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>

class PreemphasisFilter
{
public:
  PreemphasisFilter();

  void configure(float alpha, float gain);
  void reset();
  void process(float *buffer, std::size_t frames);

private:
  float alpha_;
  float gain_;
  float prev_left_;
  float prev_right_;
};
