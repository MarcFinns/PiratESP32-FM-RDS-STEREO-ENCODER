/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                     RDS 57 kHz Subcarrier Synthesizer Implementation
 *
 * =====================================================================================
 *
 * File:         RDSSynth.cpp
 * Description:  Real-time RDS baseband and modulation kernel at DAC sample rate
 *
 * Purpose:
 *   This implementation provides the core RDS signal generation pipeline. It performs
 *   bit-to-symbol Manchester encoding, timing control, baseband filtering, and
 *   up-conversion to 57 kHz in a single real-time block at DAC sample rate
 *   (Config::SAMPLE_RATE_DAC).
 *
 * Signal Flow:
 *   1. RDS bit fetching (non-blocking from RDSAssembler queue)
 *   2. Differential Manchester encoding (bi-phase mark with state)
 *   3. Symbol timing via phase accumulator (1187.5 bps = 19 kHz ÷ 16)
 *   4. Baseband shaping via cascaded IIR biquads (~2.4 kHz cutoff)
 *   5. DSB-SC modulation (×57 kHz coherent carrier)
 *   6. Amplitude scaling and output
 *
 * Manchester Encoding (Bi-Phase Mark):
 *   Each RDS bit is represented as 1 Manchester symbol (2 transitions):
 *     • Bit=0: +1 in first half, −1 in second half
 *     • Bit=1: −1 in first half, +1 in second half
 *   Differential encoding XORs incoming bits with state to add robustness.
 *
 * Symbol Timing:
 *   Uses normalized phase accumulator [0,1):
 *     sym_phase[n+1] = sym_phase[n] + sym_inc_
 *     sym_inc_ = RDS_SYMBOL_RATE / sample_rate (e.g., 1187.5 ÷ SAMPLE_RATE_DAC)
 *   At half (0.5) and full (1.0) phase, symbol transitions occur.
 *
 * Baseband Filtering (Biquad Cascade):
 *   Two cascaded second-order IIR lowpass filters targeting ~2.4 kHz:
 *     • Order 4 (2 poles/pair)
 *     • Butterworth-like response (Q ≈ 0.707)
 *     • Prevents RF splatter and aliasing into pilot/subcarrier bands
 *
 * Thread Safety:
 *   Not thread-safe. processBlockWithCarrier() must be called exclusively from
 *   Core 0 audio pipeline. configure() and reset() must not be called during
 *   processing. RDSAssembler::nextBit() is accessed via non-blocking API.
 *
 * Performance:
 *   Per-block cost (512 samples @ Config::SAMPLE_RATE_DAC):
 *     • Manchester + differential: 512 ops (negligible)
 *     • Biquad filtering: 2 × 512 × 4 = 4096 FLOPS (SIMD accelerated)
 *     • Modulation: 512 multiplications
 *     • Total: ~2–3 ms per block on ESP32-S3
 *
 * =====================================================================================
 */

#include "RDSSynth.h"

#include "Config.h"
#include "RDSAssembler.h"

#include <cstring>

#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "DSPCompat.h"

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

    // Baseband LPF design (two biquads). Target ≈ 2.4 kHz cutoff at DAC rate.
    float f = 2400.0f / sample_rate_hz; // normalized cutoff at DAC rate
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

    float sign = (last_diff_ & 1u) ? neg : one; // Differential phase state (+1/-1)
    for (std::size_t i = 0; i < samples; ++i)
    {
        // Manchester (bi‑phase mark): mid‑symbol inversion; apply differential sign
        bb[i] = sign * (half_toggle_ ? neg : one);

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

            // Fetch the next bit (non‑blocking); idle = 1 (RDS standard)
            // When queue is empty, use idle bit = 1 instead of 0.
            // This provides better clock recovery on older receivers.
            uint8_t bit = 1;
            RDSAssembler::nextBit(bit);

            // Differential encoding: d[k] = d[k-1] XOR b[k]
            last_diff_ ^= (bit & 1u);
            sign = (last_diff_ & 1u) ? neg : one;
        }
    }

    // 2) Baseband shaping via two cascaded IIR biquads (SIMD via esp‑dsp)
    DSP_BIQUAD_F32(bb, bb, (int)samples, lpf1_, w1_);
    DSP_BIQUAD_F32(bb, bb, (int)samples, lpf2_, w2_);

    // 3) DSB‑SC modulation at 57 kHz using coherent carrier and scaling
    for (std::size_t i = 0; i < samples; ++i)
    {
        out[i] = bb[i] * carrier57[i] * amp;
    }
}
} // namespace RDSSynth
