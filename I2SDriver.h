/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
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
 *     • I2S TX (Port 1): 192 kHz stereo output to DAC
 *     • I2S RX (Port 0): 48 kHz stereo input from ADC
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
 *   TX Interface (I2S_NUM_1):
 *     • Sample rate: 192 kHz
 *     • Format: 32-bit words (24-bit audio, MSB-aligned)
 *     • MCLK: 24.576 MHz (192k × 128)
 *     • BCK: 12.288 MHz (192k × 64)
 *     • DMA: 6 buffers × 240 samples = 1440 samples total
 *
 *   RX Interface (I2S_NUM_0):
 *     • Sample rate: 48 kHz
 *     • Format: 32-bit words (24-bit audio, MSB-aligned)
 *     • MCLK: 24.576 MHz (shared from TX, 48k × 512)
 *     • BCK: 3.072 MHz (48k × 64)
 *     • DMA: 6 buffers × 240 samples = 1440 samples total
 *
 * DMA Buffer Strategy:
 *   Buffer count: 6 (provides triple-buffering for TX and RX)
 *   Buffer length: 240 samples (5 ms @ 48 kHz, 1.25 ms @ 192 kHz)
 *   Total latency: ~15 ms @ 48 kHz, ~3.75 ms @ 192 kHz
 *
 * Clock Relationships:
 *   MCLK (24.576 MHz) is the master clock shared between ADC and DAC:
 *     • DAC: 24.576 MHz ÷ 128 = 192 kHz
 *     • ADC: 24.576 MHz ÷ 512 = 48 kHz
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
     * Configures I2S Port 1 for 192 kHz stereo output. Sets up DMA buffers,
     * clock generation (MCLK = 24.576 MHz), and GPIO pin assignments.
     *
     * I2S TX Configuration:
     *   • Mode: Master TX (ESP32 generates all clocks)
     *   • Sample rate: 192,000 Hz (from AudioConfig::SAMPLE_RATE_DAC)
     *   • Format: 32-bit words, 24-bit audio, stereo (L/R interleaved)
     *   • MCLK: 24.576 MHz (192k × 128) on GPIO configured in AudioConfig
     *   • BCK: 12.288 MHz (192k × 32 × 2 channels)
     *   • LRCK: 192 kHz (word select, toggles L/R)
     *   • DMA: 6 buffers × 240 samples (no queues, blocking I/O)
     *
     * Pin Assignment:
     *   Pins are read from AudioConfig namespace:
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
     * Configures I2S Port 0 for 48 kHz stereo input. Sets up DMA buffers
     * and GPIO pin assignments. MCLK is shared from TX interface.
     *
     * I2S RX Configuration:
     *   • Mode: Master RX (ESP32 generates clocks, ADC is slave)
     *   • Sample rate: 48,000 Hz (from AudioConfig::SAMPLE_RATE_ADC)
     *   • Format: 32-bit words, 24-bit audio, stereo (L/R interleaved)
     *   • MCLK: 24.576 MHz (shared from TX, 48k × 512)
     *   • BCK: 3.072 MHz (48k × 32 × 2 channels)
     *   • LRCK: 48 kHz (word select)
     *   • DMA: 6 buffers × 240 samples (5 ms latency per buffer)
     *
     * Pin Assignment:
     *   Pins are read from AudioConfig namespace:
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

} // namespace AudioIO

// =====================================================================================
//                                END OF FILE
// =====================================================================================
