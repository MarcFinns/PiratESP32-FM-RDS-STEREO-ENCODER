/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       FM Stereo Multiplex Signal Mixer Implementation
 *
 * =====================================================================================
 *
 * File:         MPXMixer.cpp
 * Description:  Real-time FM multiplex baseband signal construction
 *
 * Purpose:
 *   This implementation module provides the signal mixing kernel for constructing
 *   the complete FM stereo multiplex signal at 192 kHz. It combines mono (L+R),
 *   pilot tone (19 kHz), and stereo subcarrier (L-R modulated on 38 kHz) with
 *   configurable scaling factors.
 *
 * Algorithm:
 *   Fused accumulation in single pass over memory minimizes cache misses:
 *     MPX[i] = mono[i]
 *              + PILOT_AMP × pilot_buffer[i]
 *              + DIFF_AMP × diff[i] × subcarrier_buffer[i]
 *
 *   This approach ensures:
 *     • Deterministic latency (no intermediate buffers)
 *     • Minimal memory bandwidth (one write, multiple reads)
 *     • Compiler-friendly for vectorization
 *
 * Configuration:
 *   Pilot and subcarrier amplitudes are user-configurable to accommodate different
 *   stereo receiver standards and RF power constraints. Typical values:
 *     • pilot_amp = 0.1 (10% for ARI/RDS compatibility)
 *     • diff_amp = 0.5 (50% of mono reference for DSB-SC modulation)
 *
 * Thread Safety:
 *   Not thread-safe. Process state (amplitudes) is immutable once constructed.
 *   process() must be called exclusively from Core 0 audio task.
 *
 * Feature Gating:
 *   Audio output, pilot, and subcarrier can be individually enabled/disabled via
 *   Config namespace flags for diagnostic and testing purposes.
 *
 * =====================================================================================
 */

#include "MPXMixer.h"

MPXMixer::MPXMixer(float pilot_amp, float diff_amp)
    : pilot_amp_(pilot_amp),
      diff_amp_(diff_amp)
{
}

void MPXMixer::process(const float *mono,
                       const float *diff,
                       const float *pilot_buffer,
                       const float *subcarrier_buffer,
                       float *mpx,
                       std::size_t samples)
{
    if (!mono || !diff || !pilot_buffer || !subcarrier_buffer || !mpx || samples == 0)
    {
        return;
    }

    // Pilot and subcarrier buffers are expected to be pre-filled coherently

    // Fused accumulation: one pass over memory for best cache behavior
    for (std::size_t i = 0; i < samples; ++i)
    {
        const float mono_term  = Config::ENABLE_AUDIO ? mono[i] : 0.0f;
        const float pilot_term = Config::ENABLE_STEREO_PILOT_19K ? (pilot_amp_ * pilot_buffer[i]) : 0.0f;
        const float dsb_term   = (Config::ENABLE_AUDIO && Config::ENABLE_STEREO_SUBCARRIER_38K)
                                   ? (diff_amp_ * diff[i] * subcarrier_buffer[i])
                                   : 0.0f;
        mpx[i] = mono_term + pilot_term + dsb_term;
    }
}
