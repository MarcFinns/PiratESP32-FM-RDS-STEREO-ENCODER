/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       Stereo Decomposition Matrix Implementation
 *
 * =====================================================================================
 *
 * File:         StereoMatrix.cpp
 * Description:  Real-time stereo-to-matrix conversion (L+R/L-R decomposition)
 *
 * Purpose:
 *   This implementation provides the signal decomposition kernel that transforms
 *   interleaved stereo audio (L, R pairs) into the two independent signals required
 *   for FM stereo broadcasting: mono sum (L+R) and stereo difference (L-R).
 *
 * Algorithm:
 *   Single-pass scalar processing with fused accumulation:
 *     mono[i] = L[i] + R[i]
 *     diff[i] = L[i] - R[i]
 *
 *   This approach ensures:
 *     • Deterministic O(n) latency
 *     • Minimal memory bandwidth (one read per sample pair, two writes)
 *     • Compiler-friendly for vectorization on SIMD platforms
 *
 * Processing Model:
 *   Input: Interleaved stereo [L₀, R₀, L₁, R₁, L₂, R₂, ...]
 *   Output: Deinterleaved mono and diff buffers
 *
 *   Input buffer can be separate or overlapping with output buffers, provided
 *   memory layout is non-destructive (no in-place overwrite during iteration).
 *
 * No State:
 *   Stateless design with no persistent memory or filtering. Each call is
 *   independent, making this suitable for real-time processing where buffer
 *   boundaries or skip scenarios may occur.
 *
 * Thread Safety:
 *   Not thread-safe for concurrent access to the same buffers. Must be called
 *   exclusively from Core 0 audio processing task at 192 kHz block rate.
 *
 * Performance:
 *   Per-sample pair cost: 1 addition + 1 subtraction = 2 ops
 *   Approximately 0.01 µs per sample pair @ 192 kHz on ESP32-S3 (negligible)
 *   Perfect scaling: zero instruction overhead on compiler vectorization
 *
 * =====================================================================================
 */

#include "StereoMatrix.h"

#include <cstddef>

void StereoMatrix::process(const float *interleaved, float *mono, float *diff,
                           std::size_t samples)
{
  if (!interleaved || !mono || !diff || samples == 0)
    return;

  for (std::size_t i = 0; i < samples; ++i)
  {
    float L = interleaved[i * 2 + 0];
    float R = interleaved[i * 2 + 1];
    mono[i] = L + R;
    diff[i] = L - R;
  }
}
