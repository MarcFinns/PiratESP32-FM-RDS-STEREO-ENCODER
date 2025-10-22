/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 
 *                    19 kHz Stereo Pilot Notch Filter Interface
 *
 * =====================================================================================
 *
 * File:         NotchFilter19k.h
 * Description:  Second-order IIR notch filter to suppress 19 kHz pilot tone from input
 *
 * Purpose:
 *   This module provides a high-Q notch filter that suppresses audio components near
 *   19 kHz (the FM stereo pilot frequency) before processing. This prevents the pilot
 *   from intermodulating with pre-emphasis and other DSP stages, which could create
 *   spurious tones or interfere with RDS demodulation at the receiver.
 *
 * Filter Specifications:
 *   • Notch center frequency: 19 kHz (exact FM pilot frequency)
 *   • Sample rate: 48 kHz (input audio domain)
 *   • Quality factor: Q ≈ 25 (sharp, ~100 Hz 3dB bandwidth)
 *   • Notch depth: ~40–60 dB attenuation at 19 kHz
 *   • Passband: ±0 dB from DC to ~15 kHz (preserves audio)
 *
 * Biquad Structure:
 *   Second-order (2-pole, 2-zero) IIR filter in Direct Form II Transposed:
 *     y[n] = b₀·x[n] + b₁·x[n-1] + b₂·x[n-2] - a₁·y[n-1] - a₂·y[n-2]
 *
 *   Coefficients are generated via ESP-IDF dsps_biquad_gen_notch_f32() based on
 *   normalized frequency (f₀/fs) and quality factor Q.
 *
 * Stereo Processing:
 *   Left and right channels are filtered independently with separate state arrays
 *   (wL_[2], wR_[2]) to maintain stereo information without crosstalk.
 *
 * Latency:
 *   ~1–2 samples of group delay at 19 kHz (< 50 µs @ 48 kHz), negligible
 *   in FM multiplex context.
 *
 * Thread Safety:
 *   Not thread-safe. Must be called exclusively from Core 0 audio processing task
 *   at 48 kHz block rate. configure() must not be called while process() is active.
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>

class NotchFilter19k
{
public:
  NotchFilter19k();

  void configure(float fs, float f0, float radius);
  void reset();
  void process(float *buffer, std::size_t frames);

private:
  // dsps biquad coefficients: {b0, b1, b2, a1, a2}
  alignas(16) float coef_[5];
  // Per-channel biquad state (Direct Form I or II per dsps API)
  alignas(16) float wL_[2];
  alignas(16) float wR_[2];
};
