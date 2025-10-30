/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                         I2S Audio Driver Interface
 *
 * =====================================================================================
 *
 * File:         I2SDriver.h
 * Description:  Hardware abstraction for ESP32 I2S audio interfaces
 *
 * Purpose:
 *   This module provides a clean interface for initializing and managing the ESP32's
 *   I2S (Inter-IC Sound) peripherals. It handles dual independent I2S interfaces:
 *     • I2S TX (Port 0): DAC stereo output at Config::SAMPLE_RATE_DAC
 *     • I2S RX (Port 1): ADC stereo input at Config::SAMPLE_RATE_ADC
 *
 * I2S Architecture:
 *   The ESP32 has two I2S controllers (I2S0 and I2S1), each capable of:
 *     • Full-duplex audio (simultaneous TX and RX)
 *     • Master or slave mode
 *     • Programmable sample rates and bit depths
 *     • Hardware MCLK generation for external codec synchronization
 *     • DMA-based transfers (zero CPU intervention during transfers)
 *
 * Hardware Configuration:
 *   TX Interface (I2S_NUM_0):
 *     • Sample rate: Config::SAMPLE_RATE_DAC
 *     • Format: 32-bit words (24-bit audio, MSB-aligned)
 *     • MCLK: SAMPLE_RATE_DAC × 128
 *     • BCK:  SAMPLE_RATE_DAC × 64
 *     • DMA: 6 buffers × 256 samples = 1536 samples total
 *
 *   RX Interface (I2S_NUM_1):
 *     • Sample rate: Config::SAMPLE_RATE_ADC
 *     • Format: 32-bit words (24-bit audio, MSB-aligned)
 *     • MCLK: SAMPLE_RATE_ADC × 512 (shared from TX)
 *     • BCK:  SAMPLE_RATE_ADC × 64
 *     • DMA: 6 buffers × 64 samples = 384 samples total
 *
 * DMA Buffer Strategy:
 *   Buffer count: 6 (provides deep buffering for TX and RX)
 *   Buffer length: TX 256 samples, RX 64 samples (match block sizes)
 *   Total latency: scales with DMA settings and sample rates (TX ≈ 1536/FS, RX ≈ 384/FS)
 *
 * Clock Relationships:
 *   MCLK is the master clock shared between ADC and DAC:
 *     • MCLK = SAMPLE_RATE_DAC × 128 = SAMPLE_RATE_ADC × 512
 *     • Relationship example: MCLK = SAMPLE_RATE_DAC × 128 = SAMPLE_RATE_ADC × 512
 *   This ensures perfect sample rate synchronization.
 *
 * =====================================================================================
 */

#pragma once

#include <stdbool.h>

namespace AudioIO
{
    // ==================================================================================
    //                          I2S INITIALIZATION FUNCTIONS
    // ==================================================================================

    /**
     * Initialize I2S TX Interface (DAC Output)
     *
     * Configures I2S Port 0 for stereo output at Config::SAMPLE_RATE_DAC.
     * Sets up DMA buffers, clock generation (MCLK = SAMPLE_RATE_DAC × 128),
     * and GPIO pin assignments.
     *
     * I2S TX Configuration:
     *   • Mode: Master TX (ESP32 generates all clocks)
     *   • Sample rate: from Config::SAMPLE_RATE_DAC (e.g., 176,400 Hz)
     *   • Format: 32-bit words, 24-bit audio, stereo (L/R interleaved)
     *   • MCLK: SAMPLE_RATE_DAC × 128 on GPIO configured in Config
     *   • BCK:  SAMPLE_RATE_DAC × 32 × 2 channels
     *   • LRCK: SAMPLE_RATE_DAC (word select, toggles L/R)
     *   • DMA: 6 buffers × 240 samples (no queues, blocking I/O)
     *
     * Pin Assignment:
     *   Pins are read from Config namespace:
     *     • PIN_MCLK: Master clock output (24.576 MHz)
     *     • PIN_DAC_BCK: Bit clock (serial audio clock)
     *     • PIN_DAC_LRCK: Word select (L/R channel indicator)
     *     • PIN_DAC_DOUT: Serial audio data output
     *
     * Returns:
     *   true if initialization succeeded (driver installed, pins configured)
     *   false if driver installation or pin configuration failed
     *
     * Note: Must be called before any audio output can occur.
     *       Logs diagnostic information to Serial.
     */
    bool setupTx();

    /**
     * Initialize I2S RX Interface (ADC Input)
     *
     * Configures I2S Port 1 for stereo input at Config::SAMPLE_RATE_ADC.
     * Sets up DMA buffers and GPIO pin assignments. MCLK is shared from TX.
     *
     * I2S RX Configuration:
     *   • Mode: Master RX (ESP32 generates clocks, ADC is slave)
     *   • Sample rate: from Config::SAMPLE_RATE_ADC (e.g., 44,100 Hz)
     *   • Format: 32-bit words, 24-bit audio, stereo (L/R interleaved)
     *   • MCLK: SAMPLE_RATE_ADC × 512 (shared from TX)
     *   • BCK:  SAMPLE_RATE_ADC × 32 × 2 channels
     *   • LRCK: SAMPLE_RATE_ADC (word select)
     *   • DMA: 6 buffers × 240 samples (5 ms latency per buffer)
     *
     * Pin Assignment:
     *   Pins are read from Config namespace:
     *     • PIN_ADC_BCK: Bit clock
     *     • PIN_ADC_LRCK: Word select
     *     • PIN_ADC_DIN: Serial audio data input
     *
     * MCLK Sharing:
     *   The ADC MCLK is not configured here because it's already generated
     *   by the TX interface (setupTx() must be called first). Both ADC and
     *   DAC share the same 24.576 MHz MCLK for perfect synchronization.
     *
     * Returns:
     *   true if initialization succeeded
     *   false if driver installation or pin configuration failed
     *
     * Note: setupTx() must be called first to generate MCLK.
     *       Logs diagnostic information to Serial.
     */
    bool setupRx();

    /**
     * Shutdown I2S Interfaces
     *
     * Uninstalls both I2S TX and RX drivers, releasing DMA buffers and
     * disabling hardware peripherals. GPIO pins are freed.
     *
     * Use Cases:
     *   • Clean shutdown before entering deep sleep
     *   • Releasing resources for other applications
     *   • Reconfiguring I2S with different parameters
     *
     * Note: After calling shutdown(), setupTx() and setupRx() must be called
     *       again to resume audio operation.
     */
    void shutdown();

    /**
     * Emit a single, consolidated recap of I2S and display configuration.
     * Safe for Arduino/ESP-IDF: uses a single logger enqueue and is intended
     * to be called before the DSP task starts (no audio impact).
     */
    void emitHardwareRecap();

    /** Query configured I2S port numbers (for recap or diagnostics). */
    int getTxPort();
    int getRxPort();


} // namespace AudioIO

// =====================================================================================
//                                END OF FILE
// =====================================================================================
