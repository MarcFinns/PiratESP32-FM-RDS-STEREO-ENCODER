/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                    4× Polyphase FIR Upsampler Interface
 *
 * =====================================================================================
 *
 * File:         PolyphaseFIRUpsampler.h
 * Description:  High-performance 4× upsampling via polyphase FIR filter decomposition
 *
 * Purpose:
 *   This module implements efficient 4× upsampling (SAMPLE_RATE_ADC → SAMPLE_RATE_DAC) using a polyphase
 *   FIR filter architecture. This is essential for the FM multiplex pipeline, as the
 *   pilot tone (19 kHz), subcarrier (38 kHz), and RDS carrier (57 kHz) require the
 *   high DAC sample rate (Config::SAMPLE_RATE_DAC) for accurate synthesis.
 *
 * Algorithm: Polyphase Decomposition
 *   Traditional upsampling inserts zeros between samples, then applies a 192-tap
 *   lowpass filter, wasting 75% of multiply-accumulate operations on zero-valued data.
 *   Polyphase decomposition reorganizes the filter into 4 parallel sub-filters (phases),
 *   each with 24 taps (96 ÷ 4), operating at the input rate (ADC rate). This achieves
 *   4× computational speedup:
 *     • Traditional: 96 taps × SAMPLE_RATE_DAC
 *     • Polyphase: 24 taps × 4 phases × SAMPLE_RATE_ADC
 *
 * Filter Design:
 *   • 96-tap Kaiser-windowed sinc FIR filter (designed at runtime)
 *   • Passband: 0–15 kHz (FM audio limit + margin)
 *   • Transition: ~15–19 kHz
 *   • Stopband: ≥19 kHz (protects pilot, prevents imaging artifacts)
 *   • Attenuation: ~80 dB in stopband
 *   • Latency: 47.5 samples @ SAMPLE_RATE_ADC ≈ 0.99 ms (when 48 kHz)
 *
 * Circular Buffer Strategy:
 *   Input samples are stored in a circular delay line with mirrored wraparound to avoid
 *   boundary checks during convolution. Each sample is written to two locations
 *   (index and index - kTapsPerPhase), ensuring contiguous valid data for the
 *   convolution window.
 *
 * SIMD Optimization:
 *   The dot-product inner loop uses ESP32-S3 dsps_dotprod_f32_aes3() for 2–4× speedup
 *   over scalar multiply-accumulate operations.
 *
 * Thread Safety:
 *   Not thread-safe. Must be called exclusively from Core 0 audio processing task
 *   at input block rate. initialize() and reset() must not be called while
 *   process() is active.
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>

class PolyphaseFIRUpsampler
{
public:
  PolyphaseFIRUpsampler();

  void initialize();
  void reset();
  void process(const float *__restrict input, float *__restrict output,
               std::size_t frames);

  static constexpr std::size_t kUpsampleFactor = 4;
  static constexpr std::size_t kTaps = 96;
  static constexpr std::size_t kPhases = kUpsampleFactor;
  static constexpr std::size_t kTapsPerPhase = kTaps / kPhases;

private:
  void initPhaseCoeffs();

  alignas(16) float phase_coeffs_[kPhases][kTapsPerPhase];
  alignas(16) float state_L_[kTapsPerPhase * 2];
  alignas(16) float state_R_[kTapsPerPhase * 2];
  int state_index_;
};
