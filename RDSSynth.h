/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                     RDS 57 kHz Subcarrier Synthesizer
 *
 * =====================================================================================
 *
 * File:         RDSSynth.h
 * Description:  RDS (Radio Data System) baseband synthesizer and upconverter
 *
 * Purpose:
 *   This module generates the 57 kHz RDS injection signal synchronously within the
 *   audio processing pipeline (Core 0, 192 kHz block rate). It implements the complete
 *   RDS encoding chain:
 *     • Bit-to-symbol conversion with differential Manchester encoding (bi-phase mark)
 *     • Symbol-rate timing generation (1187.5 bps, matched to FM multiplex structure)
 *     • Baseband shaping via cascaded biquad lowpass filters (~4 kHz cutoff)
 *     • Up-conversion to 57 kHz via coherent carrier multiplication
 *
 * Signal Path:
 *   RDS bits → Differential encoder → Manchester line coder → Baseband LPF (2× biquad)
 *              → Amplitude modulation (×57 kHz carrier) → Scale → Output buffer
 *
 * Carrier Synchronization:
 *   The 57 kHz RDS carrier is derived from the master 19 kHz NCO harmonics (3× pilot).
 *   This ensures perfect frequency and phase coherence with the FM stereo pilot tone,
 *   critical for FM receiver compatibility and to prevent self-interference.
 *
 * RDS Format (RBDS):
 *   • Bit rate: 1187.5 bps (= 19 kHz pilot frequency ÷ 16)
 *   • Modulation: BPSK on 57 kHz subcarrier (binary phase shift keying)
 *   • Line code: Differential Manchester (bi-phase mark), prevents DC bias
 *   • Blocks: Group blocks (4 blocks per group), error correction via syndrome bits
 *
 * Symbol Timing (Manchester):
 *   Each RDS bit is represented as 1 Manchester symbol (2 transitions per symbol):
 *     • Symbol period: 1 ÷ 1187.5 ≈ 842 µs @ 48 kHz = 40.32 samples
 *     • At 192 kHz: 161.28 samples per symbol (not integer, so phase accumulation)
 *   The module uses a normalized phase accumulator [0,1) for continuous symbol timing.
 *
 * Baseband Shaping (Biquad Cascade):
 *   Two second-order IIR lowpass filters (Butterworth-like) with ~4 kHz cutoff:
 *     • Total order: 4 (2 poles/pair)
 *     • Purpose: Bandlimit RDS baseband to prevent aliasing and RF splatter
 *     • Coefficients: Pre-computed via dsps_biquad_gen_lpf_f32 or similar
 *
 * Thread Safety:
 *   Not thread-safe. Must be called exclusively from Core 0 audio pipeline.
 *   configure() and reset() must not be called while processBlockWithCarrier()
 *   is executing. The RDSAssembler dependency (bits) is read via non-blocking API.
 *
 * Integration:
 *   DSP_pipeline provides the coherent 57 kHz carrier and calls processBlockWithCarrier()
 *   at 192 kHz block rate. The output is mixed into the MPX signal before I2S TX.
 *
 * =====================================================================================
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

