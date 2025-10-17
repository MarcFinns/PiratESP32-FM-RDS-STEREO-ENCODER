#include "NCO.h"

#include <cmath>

namespace
{
constexpr float kTwoPi = 6.28318530717958647692f;
}

// -----------------------------------------------------------------------------
// Static LUT storage
// -----------------------------------------------------------------------------
float NCO::sin_table_[NCO::TABLE_SIZE];
bool NCO::table_init_ = false;

NCO::NCO(float freq_hz, float sample_rate)
{
    // Initialize the lookup table once (thread-safe enough for setup use)
    if (!table_init_)
    {
        init_table();
    }
    reset(); // start at 0 phase for deterministic tone start
    setFrequency(freq_hz, sample_rate);
}

void NCO::setFrequency(float freq_hz, float sample_rate)
{
    // Normalized phase increment [0,1) per sample
    // Example: 19 kHz / 192 kHz = 0.09896 cycles per sample
    phase_inc_ = (sample_rate > 0.0f) ? (freq_hz / sample_rate) : 0.0f;
}

void NCO::reset()
{
  phase_ = 0.0f;
}

void NCO::init_table()
{
    // Precompute one period of sine; cosine is obtained via 90° phase shift
    for (std::size_t i = 0; i < TABLE_SIZE; ++i)
    {
        sin_table_[i] = std::sinf(kTwoPi * static_cast<float>(i) /
                                  static_cast<float>(TABLE_SIZE));
    }
  table_init_ = true;
}

// (generate() removed in favor of generate_harmonics())

void NCO::generate_harmonics(float *pilot_out, float *sub_out, float *rds_out, std::size_t len)
{
    if (len == 0)
    {
        return;
    }
    const std::size_t mask = TABLE_SIZE - 1;
    for (std::size_t i = 0; i < len; ++i)
    {
        // Generate phase-coherent harmonics of the fundamental (19 kHz)
        // All carriers shifted by +0.25 cycles to get cosine (90° ahead of sine)
        //
        // p1: 1st harmonic (19 kHz) for pilot tone
        // p2: 2nd harmonic (38 kHz) for stereo subcarrier (L-R modulation)
        // p3: 3rd harmonic (57 kHz) for RDS subcarrier

        float p1 = phase_ + 0.25f;              // 1×phase + 90°
        if (p1 >= 1.0f) p1 -= 1.0f;

        float p2 = (phase_ * 2.0f) + 0.25f;     // 2×phase + 90°
        if (p2 >= 1.0f) p2 -= 1.0f;

        float p3 = (phase_ * 3.0f) + 0.25f;     // 3×phase + 90°
        if (p3 >= 1.0f) p3 -= 1.0f;

        auto sample_cos = [&](float pf) {
            float idx_f = pf * static_cast<float>(TABLE_SIZE);
            std::size_t idx = static_cast<std::size_t>(idx_f);
            float frac = idx_f - static_cast<float>(idx);
            float s0 = sin_table_[idx & mask];
            float s1 = sin_table_[(idx + 1) & mask];
            return s0 + frac * (s1 - s0);
        };

        float c1 = sample_cos(p1);
        float c2 = sample_cos(p2);
        float c3 = sample_cos(p3);

        if (pilot_out) pilot_out[i] = c1;
        if (sub_out) sub_out[i] = c2;
        if (rds_out) rds_out[i] = c3;

        phase_ += phase_inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
