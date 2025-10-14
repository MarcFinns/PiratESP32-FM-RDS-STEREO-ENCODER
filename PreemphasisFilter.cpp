#include "PreemphasisFilter.h"

#include <algorithm>

PreemphasisFilter::PreemphasisFilter()
    : alpha_(0.0f), gain_(1.0f), prev_left_(0.0f), prev_right_(0.0f)
{
}

void PreemphasisFilter::configure(float alpha, float gain)
{
  alpha_ = alpha;
  gain_ = gain;
  reset();
}

void PreemphasisFilter::reset()
{
  prev_left_ = 0.0f;
  prev_right_ = 0.0f;
}

void PreemphasisFilter::process(float *buffer, std::size_t frames)
{
  if (!buffer || frames == 0)
    return;

  for (std::size_t i = 0; i < frames; ++i)
  {
    float current_L = buffer[i * 2 + 0];
    float current_R = buffer[i * 2 + 1];

    float filtered_L = (current_L - alpha_ * prev_left_) * gain_;
    float filtered_R = (current_R - alpha_ * prev_right_) * gain_;

    // Clamp to [-1, 1]
    filtered_L = std::min(1.0f, std::max(-1.0f, filtered_L));
    filtered_R = std::min(1.0f, std::max(-1.0f, filtered_R));

    buffer[i * 2 + 0] = filtered_L;
    buffer[i * 2 + 1] = filtered_R;

    prev_left_ = current_L;
    prev_right_ = current_R;
  }
}

