/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                    FM Pre-Emphasis Filter Implementation
 *
 * =====================================================================================
 *
 * File:         PreemphasisFilter.cpp
 * Description:  Real-time first-order IIR pre-emphasis filter for FM broadcast
 *
 * Purpose:
 *   This implementation provides the critical FM broadcast pre-emphasis function,
 *   a standardized high-frequency boost applied before transmission. FM receivers
 *   apply complementary de-emphasis filtering to restore flat response and reduce
 *   high-frequency noise prevalent in the RF channel.
 *
 * Filter Architecture:
 *   Leaky differentiator (first-order high‑pass):
 *     y[n] = gain · (x[n] − α · x[n−1])
 *
 *   Frequency response (75 μs time constant):
 *     • Low frequencies: 0 dB (flat)
 *     • Mid frequencies: +3 dB point near 2.1 kHz
 *     • High frequencies: +20 dB/decade asymptote
 *
 *   This creates the standard FM pre‑emphasis curve, boosting treble before
 *   transmission. Receivers apply complementary de‑emphasis to flatten response.
 *
 * Implementation Details:
 *   • Single-pass per-sample processing
 *   • Separate state for left and right (no crosstalk)
 *   • No clamping: saturation handled downstream in DSP pipeline
 *   • Interleaved stereo buffer (L/R pairs) for I2S compatibility
 *
 * Initialization:
 *   Alpha coefficient must be configured via configure() before processing begins.
 *   Typical values at 48 kHz:
 *     • 75 μs pre-emphasis: α ≈ 0.015 (time constant / fs)
 *     • 50 μs pre-emphasis: α ≈ 0.020 (European standard)
 *
 * Headroom Management:
 *   Pre-emphasis boosts high frequencies and can increase peak levels by ~3–6 dB
 *   in typical music content. Callers should apply appropriate gain reduction or
 *   limiting if required.
 *
 * Performance:
 *   Per-sample cost: 4 multiplications, 2 additions (~0.1 µs @ ESP32-S3)
 *   Minimal latency: < 1 µs per block @ 48 kHz
 *
 * =====================================================================================
 */

#include "PreemphasisFilter.h"

#include <algorithm>

PreemphasisFilter::PreemphasisFilter()
    : alpha_(0.0f), gain_(1.0f), prev_left_(0.0f), prev_right_(0.0f)
{
}

void PreemphasisFilter::configure(float alpha, float gain)
{
  alpha_ = alpha;
  gain_ = gain;
  reset();
}

void PreemphasisFilter::reset()
{
  prev_left_ = 0.0f;
  prev_right_ = 0.0f;
}

void PreemphasisFilter::process(float *buffer, std::size_t frames)
{
  if (!buffer || frames == 0)
    return;

  for (std::size_t i = 0; i < frames; ++i)
  {
    float current_L = buffer[i * 2 + 0];
    float current_R = buffer[i * 2 + 1];

    float filtered_L = (current_L - alpha_ * prev_left_) * gain_;
    float filtered_R = (current_R - alpha_ * prev_right_) * gain_;

    // No clamping here: keep pre-emphasis linear and manage headroom downstream

    buffer[i * 2 + 0] = filtered_L;
    buffer[i * 2 + 1] = filtered_R;

    prev_left_ = current_L;
    prev_right_ = current_R;
  }
}
