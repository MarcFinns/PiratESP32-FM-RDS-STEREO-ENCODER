/**
 * MPXMixer implementation (Allman brace style)
 *
 * Generates pilot and subcarrier via the provided NCOs, then builds the MPX
 * with a single fused accumulation pass to minimize memory traffic:
 *   mpx = mono + (pilot_amp * pilot) + (diff_amp * diff * subcarrier)
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
        const float pilot_term = pilot_amp_ * pilot_buffer[i];
        const float dsb_term = diff_amp_ * diff[i] * subcarrier_buffer[i];
        mpx[i] = mono[i] + pilot_term + dsb_term;
    }
}
