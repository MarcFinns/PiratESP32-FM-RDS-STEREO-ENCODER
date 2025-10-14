// Stereo matrix scalar single-pass (fused L+R and L-R from interleaved stereo)
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
