#pragma once

#include <cstddef>

class StereoMatrix
{
public:
  void process(const float *interleaved, float *mono, float *diff,
               std::size_t samples);
};

