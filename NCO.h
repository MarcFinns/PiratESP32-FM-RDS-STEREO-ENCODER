#pragma once

#include <cstddef>

class NCO
{
public:
  NCO(float freq_hz, float sample_rate);

  void setFrequency(float freq_hz, float sample_rate);
  void reset();

  // Generate coherent harmonics from the master phase:
  //  pilot_out = cos(1×phase), sub_out = cos(2×phase), rds_out = cos(3×phase)
  // Any of the output pointers can be nullptr to skip generation.
  void generate_harmonics(float *pilot_out, float *sub_out, float *rds_out, std::size_t len);

  // Accessors for optional synchronization
  inline float phase() const { return phase_; }
  inline void setPhase(float p)
  {
    // wrap to [0,1)
    float ip = static_cast<float>(static_cast<int>(p));
    phase_ = p - ip;
    if (phase_ < 0.0f) phase_ += 1.0f;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
  }
  inline float phaseInc() const { return phase_inc_; }

private:
  static constexpr std::size_t TABLE_SIZE = 1024; // power-of-two
  static void init_table();
  static float sin_table_[TABLE_SIZE];
  static bool table_init_;

  // Normalized phase [0,1)
  float phase_ = 0.0f;
  // Normalized phase increment per sample [0,1)
  float phase_inc_ = 0.0f;
};
