/*
 * =====================================================================================
 *
 *                              RDS Synthesizer (Core 0)
 *
 * =====================================================================================
 */
#include "RDSSynth.h"

#include "Config.h"
#include "RDSAssembler.h"

#include <cstring>

#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"

namespace RDSSynth
{
Synth::Synth() {}

void Synth::configure(float sample_rate_hz)
{
    // Manchester symbol timing at 1187.5 bps
    sym_inc_ = (sample_rate_hz > 0.0f) ? (Config::RDS_SYMBOL_RATE / sample_rate_hz) : 0.0f;
    sym_phase_ = 0.0f;
    last_diff_ = 0;
    half_toggle_ = false;

    // Baseband LPF design (two biquads). Target ≈ 2.4 kHz cutoff at 192 kHz.
    float f = 2400.0f / sample_rate_hz; // normalized cutoff
    float q = 0.707f;                    // Butterworth‑like
    dsps_biquad_gen_lpf_f32(lpf1_, f, q);
    dsps_biquad_gen_lpf_f32(lpf2_, f, q);
    w1_[0] = w1_[1] = 0.0f;
    w2_[0] = w2_[1] = 0.0f;
}

void Synth::reset()
{
    sym_phase_ = 0.0f;
    last_diff_ = 0;
    half_toggle_ = false;
    w1_[0] = w1_[1] = 0.0f;
    w2_[0] = w2_[1] = 0.0f;
}

void Synth::processBlockWithCarrier(const float *carrier57, float amp, float *out, std::size_t samples)
{
    if (!carrier57 || !out || samples == 0)
    {
        return;
    }

    // 1) Build baseband Manchester waveform with differential encoding
    //    bb[i] ∈ {+1, −1} before shaping. Use a small stack buffer for typical block sizes.
    static constexpr float one = 1.0f;
    static constexpr float neg = -1.0f;
    float bb[512];
    if (samples > 512)
    {
        samples = 512; // skeleton safeguard; adjust if larger blocks are used
    }

    for (std::size_t i = 0; i < samples; ++i)
    {
        // First or second half of the Manchester symbol
        bb[i] = half_toggle_ ? neg : one;

        // Advance the symbol NCO, toggling mid‑symbol and stepping to next bit at 1.0
        sym_phase_ += sym_inc_;
        if (!half_toggle_ && sym_phase_ >= 0.5f)
        {
            half_toggle_ = true;
        }
        if (sym_phase_ >= 1.0f)
        {
            sym_phase_ -= 1.0f;
            half_toggle_ = false;

            // Fetch the next bit (non‑blocking); idle = 0 if none available
            uint8_t bit = 0;
            RDSAssembler::nextBit(bit);

            // Differential encoding: d[k] = d[k-1] XOR b[k]
            last_diff_ ^= (bit & 1u);
        }
    }

    // 2) Baseband shaping via two cascaded IIR biquads (SIMD via esp‑dsp)
    dsps_biquad_f32_aes3(bb, bb, (int)samples, lpf1_, w1_);
    dsps_biquad_f32_aes3(bb, bb, (int)samples, lpf2_, w2_);

    // 3) DSB‑SC modulation at 57 kHz using coherent carrier and scaling
    for (std::size_t i = 0; i < samples; ++i)
    {
        out[i] = bb[i] * carrier57[i] * amp;
    }
}
} // namespace RDSSynth

