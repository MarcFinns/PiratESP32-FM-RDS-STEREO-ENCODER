#pragma once

#include <cstddef>

class PolyphaseFIRUpsampler
{
public:
  PolyphaseFIRUpsampler();

  void initialize();
  void reset();
  void process(const float *__restrict input, float *__restrict output,
               std::size_t frames);

  static constexpr std::size_t kUpsampleFactor = 4;
  static constexpr std::size_t kTaps = 96;
  static constexpr std::size_t kPhases = kUpsampleFactor;
  static constexpr std::size_t kTapsPerPhase = kTaps / kPhases;

private:
  void initPhaseCoeffs();

  alignas(16) float phase_coeffs_[kPhases][kTapsPerPhase];
  alignas(16) float state_L_[kTapsPerPhase * 2];
  alignas(16) float state_R_[kTapsPerPhase * 2];
  int state_index_;
};

