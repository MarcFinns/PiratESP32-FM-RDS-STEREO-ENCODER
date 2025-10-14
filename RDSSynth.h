/*
 * =====================================================================================
 *
 *                              RDS Synthesizer (Core 0)
 *
 * =====================================================================================
 *
 * Purpose
 *   Generates the 57 kHz RDS injection signal synchronously inside the audio
 *   pipeline (192 kHz block rate). Reads bits from the RDSAssembler FIFO via a
 *   non‑blocking API and applies line coding + baseband shaping before
 *   up‑converting to 57 kHz.
 *
 * Signal Path
 *   bits → differential → Manchester (bi‑phase mark) → LPF (baseband) → × cos(57 kHz)
 *        → scale → add to MPX
 *
 * Notes
 *   - Coherent 57 kHz carrier (3× pilot) is precomputed in DSP_pipeline from the
 *     master NCO (19 kHz). This guarantees exact 3× phase lock.
 *   - Baseband shaping uses two biquads via esp‑dsp for low CPU cost.
 *
 */
#pragma once

#include <cstddef>

namespace RDSSynth
{
class Synth
{
public:
    Synth();

    // Configure symbol timing and baseband filters for the given sample rate (192 kHz)
    void configure(float sample_rate_hz);

    // Reset phase and filter state
    void reset();

    // Generate one 192 kHz block
    // carrier57: coherent 57 kHz cosine array (derived from master 19 kHz phase)
    // amp: injection amplitude (0.0–1.0, typically ~0.03)
    // out: output buffer to receive the modulated RDS block (length = samples)
    void processBlockWithCarrier(const float *carrier57, float amp, float *out, std::size_t samples);

private:
    // Symbol timing (Manchester bi‑phase mark at 1187.5 bps)
    float sym_phase_ = 0.0f; // [0,1)
    float sym_inc_ = 0.0f;   // symbols per sample
    unsigned char last_diff_ = 0; // differential encoder state
    bool half_toggle_ = false;    // mid‑symbol toggle for Manchester

    // Baseband shaping (two biquad sections)
    float lpf1_[5]{}; // b0 b1 b2 a1 a2
    float lpf2_[5]{};
    float w1_[2]{};   // state for section 1
    float w2_[2]{};   // state for section 2
};
} // namespace RDSSynth

