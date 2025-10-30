/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                              Main Application
 *
 * =====================================================================================
 * File:         DSPCompat.h
 * Description:  Lightweight compatibility layer to select optimal esp-dsp functions
 * depending on target board. Use provided macros instead of calling esp-dsp variants
 * directly in code.
 */

#pragma once

#include "Config.h" // brings in PROJ_TARGET_* flags
#include "dsps_biquad.h"
#include "dsps_dotprod.h"

#if defined(PROJ_TARGET_ESP32S3)
// ESP32-S3: use AES3 SIMD-optimized variants
#define DSP_DOTPROD_F32 dsps_dotprod_f32_aes3
#define DSP_BIQUAD_F32 dsps_biquad_f32_aes3
#elif defined(PROJ_TARGET_ESP32)
// Classic ESP32: use AE32 optimized variants
#define DSP_DOTPROD_F32 dsps_dotprod_f32_ae32
#define DSP_BIQUAD_F32 dsps_biquad_f32_ae32
#else
// Fallback: default to AE32 variants for compatibility
#pragma message("Unknown target: defaulting to AE32 DSP variants")
#define DSP_DOTPROD_F32 dsps_dotprod_f32_ae32
#define DSP_BIQUAD_F32 dsps_biquad_f32_ae32
#endif
