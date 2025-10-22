/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       Diagnostic Utilities and SIMD Verification
 *
 * =====================================================================================
 *
 * File:         Diagnostics.h
 * Description:  Utility functions for runtime verification and peak detection
 *
 * Purpose:
 *   This module provides diagnostic functions for verifying ESP32 SIMD capabilities
 *   at runtime and performing common signal analysis operations. It supports
 *   initialization-time verification of accelerated DSP functions.
 *
 * Functions:
 *   • verifySIMD(): Confirms that ESP32-S3 SIMD instructions (AES3, etc.) are
 *     available and working correctly. Called once during system startup.
 *   • findPeakAbs(): Locates the maximum absolute value in a buffer (useful for
 *     detecting clipping and overflow conditions).
 *
 * Thread Safety:
 *   Both functions are stateless and thread-safe (no shared state).
 *   findPeakAbs() can be called safely from any core or task context.
 *
 * SIMD Verification:
 *   verifySIMD() exercises ESP32 accelerated dot-product and biquad filters
 *   to confirm that dsps_dotprod_f32_aes3() and dsps_biquad_f32_aes3() are
 *   working correctly. If verification fails, it indicates a hardware issue or
 *   an incompatible ESP32 variant (requires ESP32-S3).
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace Diagnostics
{
void verifySIMD();
int32_t findPeakAbs(const int32_t *buffer, std::size_t samples);
}

