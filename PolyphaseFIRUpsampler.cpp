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
 *   decomposition of a 96-tap FIR filter to achieve high-quality anti-imaging
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
 *   • Design method: Kaiser-windowed sinc (A≈80 dB)
 *   • Total taps: 96
 *   • Taps per phase: 24 (96 ÷ 4 = 24)
 *   • Passband: 0–15 kHz (FM audio limit)
 *   • Transition band: ~15–19 kHz
 *   • Stopband: ≥19 kHz (pilot protection, anti-imaging)
 *   • Latency: 47.5 samples @ 48 kHz ≈ 0.99 ms
 *
 * Polyphase Decomposition:
 *   The 96-tap prototype filter H(z) is decomposed into 4 polyphase components:
 *     H(z) = E₀(z⁴) + z⁻¹·E₁(z⁴) + z⁻²·E₂(z⁴) + z⁻³·E₃(z⁴)
 *
 *   Each Eₙ(z) is a 24‑tap filter operating at 48 kHz. For each input sample,
 *   we compute 4 output samples by convolving with each of the 4 phase filters.
 *
 * Computational Cost:
 *   Traditional approach: 96 taps × 192 kHz = 18.432 MMAC/s
 *   Polyphase approach:   24 taps × 4 phases × 48 kHz = 4.608 MMAC/s
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

#include "Config.h"

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
     * 96-tap Kaiser-windowed lowpass filter designed for 4× upsampling.
     * Coefficients are stored in Q31 format (signed 32-bit fixed-point, scaled by 2^31).
     *
     * Filter Specifications:
     *   • Sample rate: 192 kHz (output rate)
     *   • Passband: 0–15 kHz (unity gain, flat in-band)
     *   • Transition: ~15–19 kHz
     *   • Stopband: ≥19 kHz (> ~80 dB attenuation)
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
        // Generated 96-tap Kaiser-windowed LPF (fc=15 kHz @ fs=192 kHz), Q31 format
        // Passband: 0–15 kHz, Transition: ~15–19 kHz, Stopband: ≥19 kHz
        // Scaled by L=4 so each phase has ≈ unity DC gain.
        -130590L,    -182011L,    -133672L,    87520L,      508817L,     1067028L,    1584804L,
        1789238L,    1384094L,    167842L,     -1833208L,   -4265942L,   -6439787L,   -7443484L,
        -6392792L,   -2763922L,   3279820L,    10688673L,   17567447L,   21520192L,   20275438L,
        12466537L,   -1645758L,   -19760552L,  -37652230L,  -49919524L,  -51300315L,  -38291946L,
        -10662068L,  27626918L,   68488573L,   100984524L,  113702849L,  97818007L,   50139638L,
        -24663109L,  -113392867L, -195651361L, -247155316L, -244529691L, -170634117L, -19237299L,
        202114486L,  471869330L,  757167905L,  1019097612L, 1219525402L, 1328126162L, 1328126162L,
        1219525402L, 1019097612L, 757167905L,  471869330L,  202114486L,  -19237299L,  -170634117L,
        -244529691L, -247155316L, -195651361L, -113392867L, -24663109L,  50139638L,   97818007L,
        113702849L,  100984524L,  68488573L,   27626918L,   -10662068L,  -38291946L,  -51300315L,
        -49919524L,  -37652230L,  -19760552L,  -1645758L,   12466537L,   20275438L,   21520192L,
        17567447L,   10688673L,   3279820L,    -2763922L,   -6392792L,   -7443484L,   -6439787L,
        -4265942L,   -1833208L,   167842L,     1384094L,    1789238L,    1584804L,    1067028L,
        508817L,     87520L,      -133672L,    -182011L,    -130590L,
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

void PolyphaseFIRUpsampler::process(const float* __restrict input, float* __restrict output,
                                    std::size_t frames)
{
    if (!input || !output || frames == 0)
        return;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        float in_L = input[frame * 2 + 0];
        float in_R = input[frame * 2 + 1];

        state_L_[state_index_]                                   = in_L;
        state_R_[state_index_]                                   = in_R;
        state_L_[state_index_ - static_cast<int>(kTapsPerPhase)] = in_L;
        state_R_[state_index_ - static_cast<int>(kTapsPerPhase)] = in_R;

        const float* __restrict baseL =
            &state_L_[state_index_ - static_cast<int>(kTapsPerPhase - 1)];
        const float* __restrict baseR =
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
                static_cast<float>(kFirCoeffsQ31[src_idx]) / Config::Q31_FULL_SCALE;
        }
    }
}
