/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                   Numerically Controlled Oscillator (NCO) with Harmonics
 *
 * =====================================================================================
 *
 * File:         NCO.h
 * Description:  Master phase-coherent tone generator for FM multiplex carriers
 *
 * Purpose:
 *   This module implements a high-performance numerically controlled oscillator (NCO)
 *   that generates coherent harmonics required for FM stereo and RDS synthesis:
 *     • 19 kHz stereo pilot tone (fundamental)
 *     • 38 kHz subcarrier for L-R modulation (2× pilot, ensures DSB-SC coherence)
 *     • 57 kHz RDS carrier (3× pilot, maintains phase lock with pilot and subcarrier)
 *
 *   The use of harmonics derived from a single master phase ensures perfect frequency
 *   and phase relationships across all carriers, critical for FM receiver compatibility.
 *
 * NCO Principles:
 *   A numerically controlled oscillator maintains a normalized phase accumulator
 *   that rotates through [0,1) representing a complete 2π rotation:
 *     phase[n+1] = (phase[n] + phase_increment) mod 1.0
 *     output = sin(2π · phase)
 *
 *   This discrete-time approach is exact and requires no trigonometric functions
 *   (phase_increment is precomputed from f_desired and f_sample at initialization).
 *
 * Waveform Synthesis:
 *   • Sine LUT: 1024-entry sine table with linear interpolation
 *   • Harmonic generation: sin(1×phase), sin(2×phase), sin(3×phase) computed
 *     from single master phase in one pass
 *   • Output range: [-1.0, 1.0]
 *
 * Thread Safety:
 *   Not thread-safe for configuration changes. setFrequency() and reset() must
 *   not be called while generate_harmonics() is executing. Safe for read-only
 *   phase access via phase() and phaseInc() getters.
 *
 * Performance:
 *   • Stateless for rapid re-tuning (pre-computed phase_inc_)
 *   • 1024-entry LUT fits in L1 cache
 *   • 12 multiplications per output sample (3 harmonics × 4 ops each)
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>

class NCO
{
public:
  NCO(float freq_hz, float sample_rate);

  void setFrequency(float freq_hz, float sample_rate);
  void reset();

  // Generate coherent harmonics from the master phase:
  //  pilot_out = sin(1×phase), sub_out = sin(2×phase), rds_out = sin(3×phase)
  // Any of the output pointers can be nullptr to skip generation.
  void generate_harmonics(float *pilot_out, float *sub_out, float *rds_out, std::size_t len);

  // Accessors for optional synchronization
  inline float phase() const { return phase_; }
  inline void setPhase(float p)
  {
    // wrap to [0,1)
    float ip = static_cast<float>(static_cast<int>(p));
    phase_ = p - ip;
    if (phase_ < 0.0f) phase_ += 1.0f;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
  }
  inline float phaseInc() const { return phase_inc_; }

private:
  static constexpr std::size_t TABLE_SIZE = 1024; // power-of-two
  static void init_table();
  static float sin_table_[TABLE_SIZE];
  static bool table_init_;

  // Normalized phase [0,1)
  float phase_ = 0.0f;
  // Normalized phase increment per sample [0,1)
  float phase_inc_ = 0.0f;
};
