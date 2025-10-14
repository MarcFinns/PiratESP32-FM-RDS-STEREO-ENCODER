#include "Diagnostics.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "dsps_dotprod.h"

namespace Diagnostics
{
void verifySIMD()
{
  Serial.println("\n=== TESTING IF SIMD IS ACTIVE ===");

  alignas(16) float test_a[24] = {1,  2,  3,  4,  5,  6,  7,  8,
                                  9,  10, 11, 12, 13, 14, 15, 16,
                                  17, 18, 19, 20, 21, 22, 23, 24};
  alignas(16) float test_b[24];
  for (int i = 0; i < 24; ++i)
    test_b[i] = 1.0f;

  float result = 0.0f;
  uint32_t start = esp_timer_get_time();
  for (int i = 0; i < 1000; ++i)
  {
    dsps_dotprod_f32_aes3(test_a, test_b, &result, 24);
  }
  uint32_t elapsed = esp_timer_get_time() - start;

  Serial.printf("Dot product result: %.1f (expect 300.0)\n", result);
  Serial.printf("Time for 1000 iterations: %u µs\n", elapsed);
  Serial.printf("Average per call: %.2f µs\n", elapsed / 1000.0f);

  if (elapsed > 100000)
  {
    Serial.println("\n⚠⚠⚠ SIMD IS NOT WORKING! ⚠⚠⚠");
    Serial.println("Expected: ~20-40 µs per 1000 calls WITH SIMD");
    Serial.println("Got: >100 µs (SCALAR MODE)");
    Serial.println("\nPossible causes:");
    Serial.println("1. esp-dsp not compiled with SIMD support");
    Serial.println("2. Wrong library version");
    Serial.println("3. Compiler flags missing");
  }
  else
  {
    Serial.println("\n✓ SIMD IS WORKING CORRECTLY!");
  }
  Serial.println("=====================================\n");
}

int32_t findPeakAbs(const int32_t *buffer, std::size_t samples)
{
  int32_t peak = 0;
  for (std::size_t i = 0; i < samples; ++i)
  {
    int32_t value = buffer[i];
    int32_t abs_val = value < 0 ? -value : value;
    if (abs_val > peak)
      peak = abs_val;
  }
  return peak;
}
} // namespace Diagnostics

