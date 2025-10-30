/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                         ESP32 Hardware Driver Implementation
 *
 * =====================================================================================
 *
 * File:         ESP32I2SDriver.h
 * Description:  Concrete I2S driver implementation for ESP32-S3
 *
 * Purpose:
 *   Implements the IHardwareDriver interface for ESP32-S3 hardware.
 *   Encapsulates all ESP32-specific I2S operations and hardware configuration.
 *
 * Features:
 *   • I2S TX: Stereo DAC output at Config::SAMPLE_RATE_DAC
 *   • I2S RX: Stereo ADC input at Config::SAMPLE_RATE_ADC
 *   • Master clock: SAMPLE_RATE_* × (128/512) (e.g., 22.5792 MHz for 44.1k family)
 *   • Configurable GPIO pins (defined in Config.h)
 *
 * Usage:
 *   ESP32I2SDriver driver;
 *   if (driver.initialize()) {
 *       // Ready for read/write operations
 *       driver.read(...);
 *       driver.write(...);
 *   }
 *   driver.shutdown();
 *
 * =====================================================================================
 */

#pragma once

#include "IHardwareDriver.h"
#include "Config.h"

#include <cstdint>
#include <driver/i2s.h>

/**
 * ESP32-S3 I2S Hardware Driver
 *
 * Provides concrete implementation of audio I/O for ESP32-S3.
 * Manages two independent I2S ports: I2S_NUM_0 (TX) and I2S_NUM_1 (RX).
 *
 * Hardware Configuration:
 *   • I2S_NUM_0: Output (DAC) @ Config::SAMPLE_RATE_DAC
 *   • I2S_NUM_1: Input (ADC) @ Config::SAMPLE_RATE_ADC
 *   • MCLK: SAMPLE_RATE_* × (128/512) (e.g., 22.5792 MHz for 44.1k family)
 *   • Format: I2S Philips, 24-bit samples in 32-bit words
 *
 * Pins (from Config.h):
 *   • PIN_MCLK:      Master clock output
 *   • PIN_ADC_BCK:   Input bit clock
 *   • PIN_ADC_LRCK:  Input word select
 *   • PIN_ADC_DIN:   Input data
 *   • PIN_DAC_BCK:   Output bit clock
 *   • PIN_DAC_LRCK:  Output word select
 *   • PIN_DAC_DOUT:  Output data
 *
 * Thread Safety:
 *   NOT thread-safe. All operations must be called from the same task context.
 *   (Designed for single-threaded audio processing)
 */
class ESP32I2SDriver : public IHardwareDriver
{
public:
    // ==================================================================================
    //                          LIFECYCLE MANAGEMENT
    // ==================================================================================

    /**
     * Constructor
     *
     * Initializes driver state to uninitialized.
     * Hardware is not configured until initialize() is called.
     */
    ESP32I2SDriver();

    /**
     * Destructor
     *
     * Ensures hardware is shut down cleanly.
     * Calls shutdown() if not already done.
     */
    ~ESP32I2SDriver();

    // Prevent copying (I2S resources should not be duplicated)
    ESP32I2SDriver(const ESP32I2SDriver&)            = delete;
    ESP32I2SDriver& operator=(const ESP32I2SDriver&) = delete;

    // ==================================================================================
    //                          IDRIVER INTERFACE IMPLEMENTATION
    // ==================================================================================

    /**
     * Initialize Hardware I/O System
     *
     * Sets up both I2S TX (DAC at Config::SAMPLE_RATE_DAC) and I2S RX (ADC at Config::SAMPLE_RATE_ADC).
     * TX is initialized before RX to establish MCLK reference.
     *
     * Sequence:
     *   1. Configure I2S TX peripheral (DAC)
     *   2. Wait 100 ms for MCLK stabilization
     *   3. Configure I2S RX peripheral (ADC)
     *   4. Mark as ready and set error state to 0
     *
     * Returns:
     *   true  - Initialization successful
     *   false - Initialization failed (I2S setup error)
     */
    bool initialize() override;

    /**
     * Shutdown Hardware I/O System
     *
     * Cleanly stops I2S peripherals and releases resources.
     * After shutdown(), initialize() must be called again for further I/O.
     */
    void shutdown() override;

    /**
     * Read Audio Data from I2S RX (ADC Input)
     *
     * Blocking read from ADC via I2S RX peripheral.
     * Blocks until DMA buffer contains requested data or timeout occurs.
     *
     * Parameters:
     *   buffer:           Destination buffer (int32_t array)
     *   buffer_bytes:     Buffer size in bytes
     *   bytes_read:       [OUT] Actual bytes read
     *   timeout_ms:       Maximum wait time (0xFFFFFFFF = forever)
     *
     * Returns:
     *   true  - Read successful
     *   false - Read failed (timeout or I2S error)
     */
    bool read(int32_t* buffer, std::size_t buffer_bytes, std::size_t& bytes_read,
              uint32_t timeout_ms = 0xFFFFFFFFU) override;

    /**
     * Write Audio Data to I2S TX (DAC Output)
     *
     * Blocking write to DAC via I2S TX peripheral.
     * Blocks until DMA buffer has space for requested data or timeout occurs.
     *
     * Parameters:
     *   buffer:           Source buffer (int32_t array)
     *   buffer_bytes:     Buffer size in bytes
     *   bytes_written:    [OUT] Actual bytes written
     *   timeout_ms:       Maximum wait time (0xFFFFFFFF = forever)
     *
     * Returns:
     *   true  - Write successful
     *   false - Write failed (timeout or I2S error)
     */
    bool write(const int32_t* buffer, std::size_t buffer_bytes, std::size_t& bytes_written,
               uint32_t timeout_ms = 0xFFFFFFFFU) override;

    /**
     * Get Current Input Sample Rate
     *
     * Returns current ADC sample rate (Config::SAMPLE_RATE_ADC)
     */
    uint32_t getInputSampleRate() const override { return kInputSampleRate; }

    /**
     * Get Current Output Sample Rate
     *
     * Returns current DAC sample rate (Config::SAMPLE_RATE_DAC)
     */
    uint32_t getOutputSampleRate() const override { return kOutputSampleRate; }

    /**
     * Check if Hardware is Ready
     *
     * Returns:
     *   true  - Initialized and ready for I/O
     *   false - Not initialized or in error state
     */
    bool isReady() const override { return is_initialized_; }

    /**
     * Get Hardware Error Status
     *
     * Returns:
     *   0 - No error
     *   non-zero - ESP error code (esp_err_t from driver/i2s.h)
     */
    int getErrorStatus() const override { return last_error_; }

    /**
     * Get last error (typed)
     */
    DriverError getLastError() const override { return last_driver_error_; }

    /**
     * Reset Hardware to Known State
     *
     * Performs soft reset of I2S peripherals.
     * Clears error flags but keeps current configuration.
     *
     * Returns:
     *   true  - Reset successful
     *   false - Reset failed
     */
    bool reset() override;


private:
    // ==================================================================================
    //                          CONSTANTS
    // ==================================================================================

    // Sample rates (from Config.h)
    static constexpr uint32_t kInputSampleRate  = Config::SAMPLE_RATE_ADC;   // 48000 Hz
    static constexpr uint32_t kOutputSampleRate = Config::SAMPLE_RATE_DAC;   // 192000 Hz

    // I2S port assignments
    static constexpr i2s_port_t kI2SPortRx = I2S_NUM_1;  // ADC (RX) input
    static constexpr i2s_port_t kI2SPortTx = I2S_NUM_0;  // DAC (TX) output

    // I2S configuration constants
    static constexpr int kI2SBitsPerSample = 32;  // 32-bit container
    static constexpr int kI2SChannels      = 2;   // Stereo

    // ==================================================================================
    //                          STATE VARIABLES
    // ==================================================================================

    bool        is_initialized_;     ///< true if hardware successfully initialized
    int         last_error_;         ///< Last platform error (esp_err_t). 0 = no error
    DriverError last_driver_error_;  ///< Last error (typed)


    // ==================================================================================
    //                          PRIVATE HELPER METHODS
    // ==================================================================================

    /**
     * Initialize I2S TX Peripheral (DAC Output)
     *
     * Sets up I2S_NUM_0 for stereo output at Config::SAMPLE_RATE_DAC.
     * Must be called before RX to establish MCLK reference.
     *
     * Returns:
     *   true  - TX initialized successfully
     *   false - TX initialization failed
     */
    bool initializeTx();

    /**
     * Initialize I2S RX Peripheral (ADC Input)
     *
     * Sets up I2S_NUM_1 for stereo input at Config::SAMPLE_RATE_ADC.
     * Must be called after TX to use MCLK established by TX.
     *
     * Returns:
     *   true  - RX initialized successfully
     *   false - RX initialization failed
     */
    bool initializeRx();

    /**
     * Shutdown I2S TX Peripheral
     *
     * Stops DMA transfers and releases I2S_NUM_0 resources.
     */
    void shutdownTx();

    /**
     * Shutdown I2S RX Peripheral
     *
     * Stops DMA transfers and releases I2S_NUM_1 resources.
     */
    void shutdownRx();
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
