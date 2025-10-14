#pragma once

#include <cstddef>

class PreemphasisFilter
{
public:
  PreemphasisFilter();

  void configure(float alpha, float gain);
  void reset();
  void process(float *buffer, std::size_t frames);

private:
  float alpha_;
  float gain_;
  float prev_left_;
  float prev_right_;
};

