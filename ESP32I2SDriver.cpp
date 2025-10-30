/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                         ESP32 Hardware Driver Implementation
 *
 * =====================================================================================
 *
 * File:         ESP32I2SDriver.cpp
 * Description:  Implementation of ESP32-S3 I2S driver
 *
 * This file contains the concrete implementation of I2S operations for ESP32-S3.
 * All I2S-specific hardware configuration and error handling is encapsulated here.
 *
 * =====================================================================================
 */

#include "Console.h"
#include "ESP32I2SDriver.h"
#include "I2SDriver.h" // AudioIO namespace (existing I2S setup code)

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_err.h>
#include <math.h>

// ==================================================================================
//                          CONSTRUCTOR / DESTRUCTOR
// ==================================================================================

namespace
{
static DriverError mapEspError(esp_err_t err, bool is_read)
{
    if (err == ESP_OK)
        return DriverError::None;
    if (err == ESP_ERR_TIMEOUT)
        return DriverError::Timeout;
    if (err == ESP_ERR_INVALID_ARG)
        return DriverError::InvalidArgument;
    if (err == ESP_ERR_INVALID_STATE)
        return DriverError::InvalidState;
    // Fallback by operation type
    return is_read ? DriverError::ReadFailed : DriverError::WriteFailed;
}
} // namespace

ESP32I2SDriver::ESP32I2SDriver()
    : is_initialized_(false), last_error_(0), last_driver_error_(DriverError::None)
{
    // State initialized to not-ready
}

ESP32I2SDriver::~ESP32I2SDriver()
{
    if (is_initialized_)
    {
        shutdown();
    }
}

// ==================================================================================
//                          PUBLIC INTERFACE IMPLEMENTATION
// ==================================================================================

bool ESP32I2SDriver::initialize()
{
    if (is_initialized_)
    {
        Console::enqueue(LogLevel::WARN, "ESP32I2SDriver already initialized");
        return true;
    }

    // Initialize TX first (establishes MCLK)
    if (!initializeTx())
    {
        Console::enqueue(LogLevel::ERROR, "ESP32I2SDriver: TX initialization failed");
        is_initialized_ = false;
        last_error_ = ESP_FAIL;
        last_driver_error_ = DriverError::IoError;
        return false;
    }

    // Wait for master clock to stabilize
    delay(500);

    // Initialize RX (uses MCLK from TX)
    if (!initializeRx())
    {
        Console::enqueue(LogLevel::ERROR, "ESP32I2SDriver: RX initialization failed");
        shutdownTx();
        is_initialized_ = false;
        last_error_ = ESP_FAIL;
        last_driver_error_ = DriverError::IoError;
        return false;
    }

    is_initialized_ = true;
    last_error_ = 0;
    last_driver_error_ = DriverError::None;
    Console::enqueue(LogLevel::INFO, "ESP32I2SDriver initialized successfully");

    return true;
}

void ESP32I2SDriver::shutdown()
{
    if (!is_initialized_)
    {
        return;
    }

    shutdownRx();
    shutdownTx();

    is_initialized_ = false;
    Console::enqueue(LogLevel::INFO, "ESP32I2SDriver shut down");
}

bool ESP32I2SDriver::read(int32_t *buffer, std::size_t buffer_bytes, std::size_t &bytes_read,
                          uint32_t timeout_ms)
{
    if (!is_initialized_)
    {
        last_error_ = ESP_ERR_INVALID_STATE;
        last_driver_error_ = DriverError::InvalidState;
        bytes_read = 0;
        return false;
    }

    if (!buffer || buffer_bytes == 0)
    {
        last_error_ = ESP_ERR_INVALID_ARG;
        last_driver_error_ = DriverError::InvalidArgument;
        bytes_read = 0;
        return false;
    }

    // Perform blocking read from I2S RX
    esp_err_t ret = i2s_read(kI2SPortRx, buffer, buffer_bytes, &bytes_read, timeout_ms);

    if (ret != ESP_OK)
    {
        last_error_ = ret;
        last_driver_error_ = mapEspError(ret, /*is_read=*/true);
        return false;
    }

    last_error_ = 0;
    last_driver_error_ = DriverError::None;
    return true;
}

bool ESP32I2SDriver::write(const int32_t *buffer, std::size_t buffer_bytes,
                           std::size_t &bytes_written, uint32_t timeout_ms)
{
    if (!is_initialized_)
    {
        last_error_ = ESP_ERR_INVALID_STATE;
        last_driver_error_ = DriverError::InvalidState;
        bytes_written = 0;
        return false;
    }
    if (!buffer || buffer_bytes == 0)
    {
        last_error_ = ESP_ERR_INVALID_ARG;
        last_driver_error_ = DriverError::InvalidArgument;
        bytes_written = 0;
        return false;
    }
    esp_err_t ret = i2s_write(kI2SPortTx, const_cast<int32_t *>(buffer), buffer_bytes,
                              &bytes_written, timeout_ms);
    if (ret != ESP_OK)
    {
        last_error_ = ret;
        last_driver_error_ = mapEspError(ret, /*is_read=*/false);
        return false;
    }
    last_error_ = 0;
    last_driver_error_ = DriverError::None;
    return true;
}

bool ESP32I2SDriver::reset()
{
    if (!is_initialized_)
    {
        return false;
    }

    // Stop, flush, and restart both I2S ports to clear DMA state
    i2s_stop(kI2SPortTx);
    i2s_zero_dma_buffer(kI2SPortTx);
    i2s_start(kI2SPortTx);

    i2s_stop(kI2SPortRx);
    i2s_zero_dma_buffer(kI2SPortRx);
    i2s_start(kI2SPortRx);

    last_error_ = 0;
    last_driver_error_ = DriverError::None;
    return true;
}


// ==================================================================================
//                          PRIVATE IMPLEMENTATION
// ==================================================================================

bool ESP32I2SDriver::initializeTx()
{
    // Use existing AudioIO::setupTx() function
    // This encapsulates all TX-specific configuration
    return AudioIO::setupTx();
}

bool ESP32I2SDriver::initializeRx()
{
    // Use existing AudioIO::setupRx() function
    // This encapsulates all RX-specific configuration
    return AudioIO::setupRx();
}

void ESP32I2SDriver::shutdownTx()
{
    // Driver cleanup for TX
    // The ESP32 I2S driver will handle resource cleanup
    i2s_driver_uninstall(kI2SPortTx);
}

void ESP32I2SDriver::shutdownRx()
{
    // Driver cleanup for RX
    // The ESP32 I2S driver will handle resource cleanup
    i2s_driver_uninstall(kI2SPortRx);
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
