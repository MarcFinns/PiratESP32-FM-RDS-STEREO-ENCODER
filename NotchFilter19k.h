#pragma once

#include <cstddef>

class NotchFilter19k
{
public:
  NotchFilter19k();

  void configure(float fs, float f0, float radius);
  void reset();
  void process(float *buffer, std::size_t frames);

private:
  // dsps biquad coefficients: {b0, b1, b2, a1, a2}
  alignas(16) float coef_[5];
  // Per-channel biquad state (Direct Form I or II per dsps API)
  alignas(16) float wL_[2];
  alignas(16) float wR_[2];
};
