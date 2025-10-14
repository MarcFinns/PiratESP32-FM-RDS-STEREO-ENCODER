/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                  Polyphase FIR Upsampler Implementation
 *
 * =====================================================================================
 *
 * File:         PolyphaseFIRUpsampler.cpp
 * Description:  High-performance 4× upsampling using polyphase FIR filter
 *
 * Purpose:
 *   This module implements a computationally-efficient 4× upsampler that converts
 *   48 kHz stereo audio to 192 kHz for FM multiplex synthesis. It uses a polyphase
 *   decomposition of a 79-tap FIR filter to achieve high-quality anti-imaging
 *   filtering while minimizing computational cost.
 *
 * Algorithm: Polyphase FIR Filtering
 *   Traditional upsampling inserts zeros between samples, then applies a lowpass
 *   filter to remove imaging artifacts. However, this wastes 75% of multiplications
 *   on zero-valued samples. The polyphase approach reorganizes the filter into
 *   4 sub-filters (phases), each operating at the input rate (48 kHz), eliminating
 *   wasted multiply-accumulate operations.
 *
 * Filter Design:
 *   • Design method: Parks-McClellan (equiripple optimal FIR)
 *   • Total taps: 79
 *   • Taps per phase: 20 (79 ÷ 4 ≈ 20, rounded up)
 *   • Passband: 0-20 kHz (preserves full audio bandwidth)
 *   • Transition band: 20-28 kHz
 *   • Stopband: 28+ kHz (> 80 dB attenuation, removes imaging)
 *   • Latency: 39.5 samples @ 48 kHz ≈ 0.82 ms
 *
 * Polyphase Decomposition:
 *   The 79-tap prototype filter H(z) is decomposed into 4 polyphase components:
 *     H(z) = E₀(z⁴) + z⁻¹·E₁(z⁴) + z⁻²·E₂(z⁴) + z⁻³·E₃(z⁴)
 *
 *   Each Eₙ(z) is a ~20-tap filter operating at 48 kHz. For each input sample,
 *   we compute 4 output samples by convolving with each of the 4 phase filters.
 *
 * Computational Cost:
 *   Traditional approach: 79 taps × 192 kHz = 15.168 MMAC/s
 *   Polyphase approach:   20 taps × 4 phases × 48 kHz = 3.84 MMAC/s
 *   Speedup: ~4× reduction (eliminates multiply-by-zero operations)
 *
 * Circular Buffer Management:
 *   Input samples are stored in a circular delay line. To avoid boundary checks
 *   during convolution, the buffer uses mirrored wraparound: each sample is
 *   written to two locations (index and index - kTapsPerPhase). This ensures
 *   the convolution window always has contiguous valid data.
 *
 * ESP32 SIMD Optimization:
 *   The dot-product inner loop uses ESP32-S3 dsps_dotprod_f32_aes3(), which
 *   leverages hardware SIMD instructions for 2-4× speedup over scalar code.
 *
 * =====================================================================================
 */

#include "PolyphaseFIRUpsampler.h"

#include "AudioConfig.h"

#include "dsps_dotprod.h"

#include <cstring>

// ==================================================================================
//                          FIR FILTER COEFFICIENTS
// ==================================================================================

namespace
{
/**
 * Prototype FIR Filter Coefficients (Q31 Fixed-Point)
 *
 * 79-tap Parks-McClellan equiripple lowpass filter designed for 4× upsampling.
 * Coefficients are stored in Q31 format (signed 32-bit fixed-point, scaled by 2^31).
 *
 * Filter Specifications:
 *   • Sample rate: 192 kHz (output rate)
 *   • Passband: 0-20 kHz (flat ±0.01 dB)
 *   • Transition: 20-28 kHz
 *   • Stopband: 28+ kHz (> -80 dB attenuation)
 *   • Linear phase (symmetric coefficients)
 *
 * Coefficient Organization:
 *   Coefficients are stored in prototype filter order (not polyphase order).
 *   They will be reorganized into 4 polyphase sub-filters during initialization.
 *
 * Memory Alignment:
 *   16-byte alignment enables potential SIMD optimizations during coefficient loading.
 *
 * Design Tool:
 *   Generated using MATLAB/Python scipy.signal.remez() or similar optimal FIR design.
 */
alignas(16) const int32_t kFirCoeffsQ31[PolyphaseFIRUpsampler::kTaps] = {
    -54527L,     45674L,      282154L,     587292L,     777009L,
    605313L,     -103968L,    -1294755L,   -2574295L,   -3260905L,
    -2628577L,   -296647L,    3392595L,    7223100L,    9356357L,
    8011022L,    2381217L,    -6587274L,   -16002833L,  -21711592L,
    -19818023L,  -8583003L,   10079981L,   30329509L,   43825882L,
    42700088L,   23152597L,   -11748084L,  -51506682L,  -80697786L,
    -84208851L,  -53675456L,  7302676L,    81605813L,   142168718L,
    160398888L,  117182041L,  12786269L,   -128269174L, -260101607L,
    -326420220L, -277007070L, -84894365L,  241413717L,  655244498L,
    1080393627L, 1429313199L, 1625853754L, 1625853754L, 1429313199L,
    1080393627L, 655244498L,  241413717L,  -84894365L,  -277007070L,
    -326420220L, -260101607L, -128269174L, 12786269L,   117182041L,
    160398888L,  142168718L,  81605813L,   7302676L,    -53675456L,
    -84208851L,  -80697786L,  -51506682L,  -11748084L,  23152597L,
    42700088L,   43825882L,   30329509L,   10079981L,   -8583003L,
    -19818023L,  -21711592L,  -16002833L,  -6587274L,   2381217L,
    8011022L,    9356357L,    7223100L,    3392595L,    -296647L,
    -2628577L,   -3260905L,   -2574295L,   -1294755L,   -103968L,
    605313L,     777009L,     587292L,     282154L,     45674L,
    -54527L
};
} // namespace

PolyphaseFIRUpsampler::PolyphaseFIRUpsampler() : state_index_(kTapsPerPhase)
{
  std::memset(phase_coeffs_, 0, sizeof(phase_coeffs_));
  reset();
}

void PolyphaseFIRUpsampler::initialize()
{
  initPhaseCoeffs();
  reset();
}

void PolyphaseFIRUpsampler::reset()
{
  std::memset(state_L_, 0, sizeof(state_L_));
  std::memset(state_R_, 0, sizeof(state_R_));
  state_index_ = static_cast<int>(kTapsPerPhase);
}

void PolyphaseFIRUpsampler::process(const float *__restrict input,
                                    float *__restrict output,
                                    std::size_t frames)
{
  if (!input || !output || frames == 0)
    return;

  for (std::size_t frame = 0; frame < frames; ++frame)
  {
    float in_L = input[frame * 2 + 0];
    float in_R = input[frame * 2 + 1];

    state_L_[state_index_] = in_L;
    state_R_[state_index_] = in_R;
    state_L_[state_index_ - static_cast<int>(kTapsPerPhase)] = in_L;
    state_R_[state_index_ - static_cast<int>(kTapsPerPhase)] = in_R;

    const float *__restrict baseL =
        &state_L_[state_index_ - static_cast<int>(kTapsPerPhase - 1)];
    const float *__restrict baseR =
        &state_R_[state_index_ - static_cast<int>(kTapsPerPhase - 1)];

    std::size_t out_base = frame * kUpsampleFactor * 2;
    for (std::size_t phase = 0; phase < kPhases; ++phase)
    {
      float yL = 0.0f;
      float yR = 0.0f;
      dsps_dotprod_f32_aes3(baseL, phase_coeffs_[phase], &yL,
                            static_cast<int>(kTapsPerPhase));
      dsps_dotprod_f32_aes3(baseR, phase_coeffs_[phase], &yR,
                            static_cast<int>(kTapsPerPhase));

      output[out_base + phase * 2 + 0] = yL;
      output[out_base + phase * 2 + 1] = yR;
    }

    ++state_index_;
    if (state_index_ >= static_cast<int>(kTapsPerPhase * 2))
      state_index_ = static_cast<int>(kTapsPerPhase);
  }
}

void PolyphaseFIRUpsampler::initPhaseCoeffs()
{
  for (std::size_t phase = 0; phase < kPhases; ++phase)
  {
    for (std::size_t t = 0; t < kTapsPerPhase; ++t)
    {
      std::size_t src_idx =
          (kTapsPerPhase - 1 - t) * kPhases + phase; // oldest -> newest order
      phase_coeffs_[phase][t] =
          static_cast<float>(kFirCoeffsQ31[src_idx]) / AudioConfig::Q31_FULL_SCALE;
    }
  }
}

