/**
 * MPXMixer
 *
 * Builds the FM stereo MPX baseband signal at 192 kHz:
 *   MPX = (L+R) + PILOT_AMP * 19 kHz pilot + DIFF_AMP * (L-R) * 38 kHz subcarrier
 *
 * Design goals:
 * - Deterministic and allocation‑free
 * - Minimize memory traffic (fused accumulation)
 * - Allow externally managed tone generation (NCOs)
 */
#pragma once

#include <cstddef>

#include "AudioConfig.h"

class MPXMixer
{
public:
    /** Construct a mixer with pilot and DSB scaling. */
    MPXMixer(float pilot_amp, float diff_amp);

    /**
     * Process one 192 kHz block.
     *
     * @param mono                Contiguous (L+R) buffer [samples]
     * @param diff                Contiguous (L-R) buffer [samples]
     * @param pilot_nco           NCO generating 19 kHz tone
     * @param subcarrier_nco      NCO generating 38 kHz tone
     * @param pilot_buffer        Scratch for pilot tone [samples]
     * @param subcarrier_buffer   Scratch for subcarrier [samples]
     * @param mpx                 Output MPX buffer [samples]
     * @param samples             Number of mono/diff/MPX samples in this block
     */
    void process(const float *mono,
                 const float *diff,
                 const float *pilot_buffer,
                 const float *subcarrier_buffer,
                 float *mpx,
                 std::size_t samples);

private:
    float pilot_amp_;
    float diff_amp_;
};
