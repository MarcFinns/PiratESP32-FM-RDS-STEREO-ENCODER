/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       Stereo Decomposition Matrix (L±R)
 *
 * =====================================================================================
 *
 * File:         StereoMatrix.h
 * Description:  Real-time conversion of stereo audio to L+R/L-R format for FM multiplex
 *
 * Purpose:
 *   This module decomposes interleaved stereo audio (L, R pairs) into two independent
 *   signals required for FM stereo broadcasting:
 *     • Mono sum: M = L + R (baseband 0–15 kHz, full-range amplitude)
 *     • Stereo difference: S = L - R (modulated onto 38 kHz, half-amplitude for DSB-SC)
 *
 *   FM receivers reconstruct stereo via:
 *     L = (M + S) / 2
 *     R = (M - S) / 2
 *
 *   This matrix is the critical link between the audio input domain and the multiplex
 *   synthesis pipeline.
 *
 * Processing:
 *   Input:  Interleaved stereo samples [L₀, R₀, L₁, R₁, L₂, R₂, ...]
 *   Output: Two contiguous mono buffers (mono[], diff[]) containing DAC-rate samples
 *
 *   The operation is stateless and allocation-free (no buffers allocated internally).
 *   Suitable for real-time processing on Core 0 at DAC block rate.
 *
 * Design:
 *   • Direct matrix computation: 2 operations per sample pair (1 add, 1 subtract)
 *   • In-place processing supported (if mono and diff don't overlap input)
 *   • Deterministic: O(n) complexity, no branching or recursion
 *
 * Thread Safety:
 *   Not thread-safe. Must be called exclusively from Core 0 audio processing task
 *   at DAC block rate. No shared state between tasks or cores.
 *
 * Amplitude Scaling:
 *   The mono and difference outputs are typically scaled differently before mixing:
 *     • Mono: unity gain (full 0 dB)
 *     • Difference: −6 dB (0.5×) for DSB-SC modulation onto subcarrier
 *   This scaling is done by MPXMixer, not by this module.
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>

class StereoMatrix
{
public:
  void process(const float *interleaved, float *mono, float *diff,
               std::size_t samples);
};
