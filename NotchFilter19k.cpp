#include "NotchFilter19k.h"

#include <cmath>
#include <cstring>

#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"

NotchFilter19k::NotchFilter19k()
{
  reset();
}

void NotchFilter19k::configure(float fs, float f0, float radius)
{
    // Configure a narrow/deep notch near the stereo pilot (19 kHz at 48 kHz domain).
    // We map the pole radius r to an approximate Q using Q ≈ 1/(2*(1‑r)).
    // Higher r → narrower, deeper notch with minimal passband impact.
    float f_norm = f0 / fs; // normalized frequency [0..0.5]
    float Q = (radius < 1.0f && radius > 0.0f)
                  ? (1.0f / (2.0f * (1.0f - radius)))
                  : 25.0f; // sensible default if r out of range

    // Generate biquad coefficients (b0, b1, b2, a1, a2), unity gain at DC.
    dsps_biquad_gen_notch_f32(coef_, f_norm, 1.0f, Q);
    reset();
}

void NotchFilter19k::reset()
{
  std::memset(wL_, 0, sizeof(wL_));
  std::memset(wR_, 0, sizeof(wR_));
}

void NotchFilter19k::process(float *buffer, std::size_t frames)
{
    if (!buffer || frames == 0)
    {
        return;
    }

    // Deinterleave to contiguous left/right windows for SIMD biquad.
    // At 48 kHz with small block sizes we can use small stack arrays.
    const int N = static_cast<int>(frames);
    float left[64];
    float right[64];

    // Guard: if configuration changes to larger blocks, revisit this allocation.
    if (frames > 64)
    {
        // Conservative fallback (should not happen with configured BLOCK_SIZE)
        for (std::size_t i = 0; i < frames; ++i)
        {
            buffer[i * 2 + 0] = buffer[i * 2 + 0];
            buffer[i * 2 + 1] = buffer[i * 2 + 1];
        }
        return;
    }

    for (int i = 0; i < N; ++i)
    {
        left[i] = buffer[i * 2 + 0];
        right[i] = buffer[i * 2 + 1];
    }

    // Apply notch per channel; states (wL_/wR_) keep continuity across blocks.
    dsps_biquad_f32_aes3(left, left, N, coef_, wL_);
    dsps_biquad_f32_aes3(right, right, N, coef_, wR_);

    for (int i = 0; i < N; ++i)
    {
        buffer[i * 2 + 0] = left[i];
        buffer[i * 2 + 1] = right[i];
    }
}
