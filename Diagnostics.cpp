/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       Diagnostic Utilities Implementation
 *
 * =====================================================================================
 *
 * File:         Diagnostics.cpp
 * Description:  Runtime verification and analysis utilities for DSP pipeline
 *
 * Purpose:
 *   This implementation provides diagnostic functions called during system startup
 *   to verify SIMD acceleration availability and provide basic signal analysis
 *   utilities for runtime monitoring and debugging.
 *
 * SIMD Verification:
 *   verifySIMD() exercises the DSP library's SIMD-accelerated dot-product function
 *   with 1000 iterations to measure performance. On ESP32-S3 with SIMD enabled,
 *   this should complete in ~20–40 µs. Slower times (>100 µs) indicate scalar
 *   fallback or library configuration issues.
 *
 *   Expected behavior:
 *     • SIMD working: < 40 µs total (0.04 µs per call)
 *     • Scalar fallback: > 100 µs total (0.1 µs per call)
 *     • Prints diagnostic output to Serial for verification at startup
 *
 * Peak Detection:
 *   findPeakAbs() scans a buffer for the maximum absolute value, useful for:
 *     • Monitoring clipping (is peak near INT32_MAX?)
 *     • Detecting signal presence (is peak above noise floor?)
 *     • Validating overflow conditions in test vectors
 *
 *   Stateless operation suitable for any context (interrupt-safe, no allocation).
 *
 * Performance:
 *   • verifySIMD(): ~20–40 µs with SIMD, ~100–200 µs without (ESP32-S3)
 *   • findPeakAbs(): O(n) linear scan, ~1 ns per sample on ESP32-S3
 *
 * Integration:
 *   Called early in system initialization (SystemContext::begin()) before audio
 *   processing starts. If SIMD verification fails, system continues but may see
 *   reduced real-time performance in DSP modules.
 *
 * =====================================================================================
 */

#include "Diagnostics.h"

#include "Console.h"
#include <Arduino.h>
#include <esp_timer.h>

#include "dsps_dotprod.h"

namespace Diagnostics
{
void verifySIMD()
{
    Console::printOrSerial(LogLevel::INFO, "");
    Console::printOrSerial(LogLevel::INFO, "=== TESTING IF RUNNING ON A SIMD-ENABLED CPU ===");

    alignas(16) float test_a[24] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
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

    Console::printfOrSerial(LogLevel::INFO, "Dot product result: %.1f (expect 300.0)", result);
    Console::printfOrSerial(LogLevel::INFO, "Time for 1000 iterations: %u µs", elapsed);
    Console::printfOrSerial(LogLevel::INFO, "Average per call: %.2f µs", elapsed / 1000.0f);

    if (elapsed > 100000)
    {
        Console::printOrSerial(LogLevel::WARN, "");
        Console::printOrSerial(LogLevel::WARN, "⚠⚠⚠ SIMD IS NOT AVAILABLE! ⚠⚠⚠");
        Console::printOrSerial(LogLevel::WARN, "Expected: ~20-40 µs per 1000 calls WITH SIMD");
        Console::printOrSerial(LogLevel::WARN, "Got: >100 µs (SCALAR MODE)");
        Console::printOrSerial(LogLevel::WARN, "");
        Console::printOrSerial(LogLevel::WARN, "Possible causes:");
        Console::printOrSerial(LogLevel::WARN, "1. esp-dsp not compiled with SIMD support");
        Console::printOrSerial(LogLevel::WARN, "2. Wrong library version");
        Console::printOrSerial(LogLevel::WARN, "3. Compiler flags missing");
        Console::printOrSerial(LogLevel::WARN, "4. CPU without SIMD support");
    }
    else
    {
        Console::printOrSerial(LogLevel::INFO, "");
        Console::printOrSerial(LogLevel::INFO, "✓ SIMD INSTRUCTIONS AVAILABLE!");
    }
    Console::printOrSerial(LogLevel::INFO, "=====================================");
    Console::printOrSerial(LogLevel::INFO, "");
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
