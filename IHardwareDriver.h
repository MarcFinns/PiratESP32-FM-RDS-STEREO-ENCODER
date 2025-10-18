/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                       Hardware Abstraction Layer (Interface)
 *
 * =====================================================================================
 *
 * File:         IHardwareDriver.h
 * Description:  Pure virtual interface for hardware I/O operations
 *
 * Purpose:
 *   Defines the contract for hardware drivers. This allows DSP_pipeline to work
 *   with different hardware implementations without modification.
 *
 * Design Pattern:
 *   • Interface Segregation: Minimal, focused interface
 *   • Dependency Inversion: DSP_pipeline depends on abstraction, not concrete driver
 *   • Testability: Mock implementations can be injected for unit testing
 *
 * Implementations:
 *   • ESP32I2SDriver: Hardware I2S driver for ESP32-S3
 *   • MockHardwareDriver: For testing (future)
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * DriverError - Typed hardware driver error codes
 *
 * Provides a platform-agnostic summary of the last failure cause while still
 * allowing implementations to expose platform-specific details separately
 * (e.g., ESP-IDF's esp_err_t via getErrorStatus()).
 */
enum class DriverError : uint8_t
{
    None = 0,

    // Parameter/State
    InvalidArgument,
    InvalidState,
    NotInitialized,

    // I/O specific
    Timeout,
    ReadFailed,
    WriteFailed,
    IoError,

    // Unknown/other
    Unknown = 255
};

/**
 * Hardware Driver Interface
 *
 * Pure virtual interface for audio I/O hardware abstraction.
 * Provides minimal, focused API for I2S operations required by DSP_pipeline.
 *
 * Thread Safety:
 *   All operations are designed to be called from a single task context (audio task).
 *   Not thread-safe for concurrent operations.
 */
class IHardwareDriver
{
public:
    virtual ~IHardwareDriver() = default;

    // ==================================================================================
    //                          INITIALIZATION
    // ==================================================================================

    /**
     * Initialize Hardware I/O System
     *
     * Sets up both input (ADC) and output (DAC) I2S peripherals.
     * This is the main initialization entry point.
     *
     * Initialization Order:
     *   1. TX peripheral (192 kHz DAC output) - must be first for MCLK stability
     *   2. Wait ~100 ms for master clock stabilization
     *   3. RX peripheral (48 kHz ADC input)
     *   4. Verify I2S channels are ready for operation
     *
     * Returns:
     *   true  - All hardware initialized successfully, ready for read/write
     *   false - Initialization failed (check logs for details)
     *
     * Note: Must be called once during system startup, before first read/write.
     */
    virtual bool initialize() = 0;

    /**
     * Shutdown Hardware I/O System
     *
     * Cleanly shuts down I2S peripherals and releases hardware resources.
     * Called during system shutdown or reinitialization.
     *
     * Operations:
     *   • Stop DMA transfers
     *   • Release I2S port resources
     *   • Return GPIO pins to default state
     *
     * Note: After calling shutdown(), initialize() must be called again
     *       before further read/write operations.
     */
    virtual void shutdown() = 0;

    // ==================================================================================
    //                          AUDIO I/O OPERATIONS
    // ==================================================================================

    /**
     * Read Audio Data from Input (ADC via I2S RX)
     *
     * Blocking read of one audio block from the I2S RX peripheral.
     * This is the primary path for capturing input audio.
     *
     * Buffer Format:
     *   • int32_t array, interleaved stereo: [L0, R0, L1, R1, L2, R2, ...]
     *   • Q31 fixed-point format (24-bit audio in 32-bit container)
     *   • Stereo pairs only: frames_to_read = bytes / (2 * 4)
     *
     * Parameters:
     *   buffer:           Destination buffer (int32_t array)
     *   buffer_bytes:     Size of buffer in bytes (must be multiple of 8)
     *   bytes_read:       [OUT] Actual bytes read (updated by implementation)
     *   timeout_ms:       Maximum wait time in milliseconds (0 = wait forever)
     *
     * Returns:
     *   true  - Data read successfully (check bytes_read for amount)
     *   false - Read failed (timeout, I2S error, or hardware issue)
     *
     * Blocking Behavior:
     *   • Blocks until DMA buffer contains requested amount of data
     *   • Typical block time: ~1.33 ms @ 48 kHz (one audio block period)
     *   • Can timeout if I2S clock is stopped or missing
     *
     * Error Handling:
     *   • Returns false on timeout or hardware error
     *   • bytes_read indicates partial data if applicable
     *
     * Thread Safety:
     *   Safe only when called from single task context (audio task).
     */
    virtual bool read(int32_t* buffer, std::size_t buffer_bytes, std::size_t& bytes_read,
                      uint32_t timeout_ms = 0xFFFFFFFFU) = 0;

    /**
     * Write Audio Data to Output (DAC via I2S TX)
     *
     * Blocking write of one audio block to the I2S TX peripheral.
     * This is the primary path for transmitting processed audio.
     *
     * Buffer Format:
     *   • int32_t array, interleaved stereo: [L0, R0, L1, R1, L2, R2, ...]
     *   • Q31 fixed-point format (24-bit audio in 32-bit container)
     *   • Stereo pairs only: frames_to_write = bytes / (2 * 4)
     *
     * Parameters:
     *   buffer:           Source buffer (int32_t array, read-only)
     *   buffer_bytes:     Size of buffer in bytes (must be multiple of 8)
     *   bytes_written:    [OUT] Actual bytes written (updated by implementation)
     *   timeout_ms:       Maximum wait time in milliseconds (0 = wait forever)
     *
     * Returns:
     *   true  - Data written successfully (check bytes_written for confirmation)
     *   false - Write failed (timeout, I2S error, or DAC issue)
     *
     * Blocking Behavior:
     *   • Blocks until DMA buffer has space for requested amount of data
     *   • Typical block time: ~1.33 ms @ 192 kHz (one audio block period)
     *   • Underrun if write lags behind DAC consumption
     *
     * Underrun Handling:
     *   • If bytes_written < buffer_bytes, indicates underrun (DSP too slow)
     *   • Log warning and skip this block to maintain timing
     *   • Repeated underruns indicate CPU overload
     *
     * Thread Safety:
     *   Safe only when called from single task context (audio task).
     */
    virtual bool write(const int32_t* buffer, std::size_t buffer_bytes, std::size_t& bytes_written,
                       uint32_t timeout_ms = 0xFFFFFFFFU) = 0;

    // ==================================================================================
    //                          CONFIGURATION & DIAGNOSTICS
    // ==================================================================================

    /**
     * Get Current Input Sample Rate
     *
     * Returns the configured I2S RX sample rate in Hz.
     *
     * Returns:
     *   Sample rate in Hz (e.g., 48000 for 48 kHz)
     *
     * Note: This is informational only. Sample rate is configured at
     *       initialization time and typically cannot be changed at runtime.
     */
    virtual uint32_t getInputSampleRate() const = 0;

    /**
     * Get Current Output Sample Rate
     *
     * Returns the configured I2S TX sample rate in Hz.
     *
     * Returns:
     *   Sample rate in Hz (e.g., 192000 for 192 kHz)
     *
     * Note: This is informational only. Sample rate is configured at
     *       initialization time and typically cannot be changed at runtime.
     */
    virtual uint32_t getOutputSampleRate() const = 0;

    /**
     * Check if Hardware is Ready
     *
     * Returns true if hardware is initialized and ready for I/O.
     * Useful for pre-flight checks before processing.
     *
     * Returns:
     *   true  - Hardware initialized and ready
     *   false - Hardware not initialized or in error state
     *
     * Note: This is a status check only and does not modify state.
     */
    virtual bool isReady() const = 0;

    /**
     * Get Hardware Error Status
     *
     * Returns a driver-specific error code if hardware is in an error state.
     * Used for diagnostics and error logging.
     *
     * Returns:
     *   0 - No error
     *   non-zero - Driver-specific error code (implementation defined)
     *
     * Note: Error codes are driver-specific and may vary by implementation.
     *       See implementation documentation for error code meanings.
     */
    virtual int getErrorStatus() const = 0;

    /**
     * Get last error (typed)
     *
     * Platform-agnostic error classification describing the last failure.
     * This complements getErrorStatus() which returns the platform-specific
     * status code (e.g., esp_err_t for ESP-IDF).
     */
    virtual DriverError getLastError() const = 0;

    /**
     * Reset Hardware to Known State
     *
     * Performs a soft reset of the hardware I/O system.
     * Clears error flags and resets DMA buffers without full reinitialization.
     *
     * Returns:
     *   true  - Reset successful
     *   false - Reset failed
     *
     * Note: This does NOT stop I/O. Call shutdown() first to stop I/O
     *       before calling reset() if required.
     */
    virtual bool reset() = 0;
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
