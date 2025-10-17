/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
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

#include "ESP32I2SDriver.h"
#include "I2SDriver.h"  // AudioIO namespace (existing I2S setup code)
#include "Log.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_err.h>

// ==================================================================================
//                          CONSTRUCTOR / DESTRUCTOR
// ==================================================================================

ESP32I2SDriver::ESP32I2SDriver()
    : is_initialized_(false), last_error_(0)
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
        Log::enqueue(Log::WARN, "ESP32I2SDriver already initialized");
        return true;
    }

    // Initialize TX first (establishes MCLK)
    if (!initializeTx())
    {
        Log::enqueue(Log::ERROR, "ESP32I2SDriver: TX initialization failed");
        is_initialized_ = false;
        return false;
    }

    // Wait for master clock to stabilize
    delay(100);

    // Initialize RX (uses MCLK from TX)
    if (!initializeRx())
    {
        Log::enqueue(Log::ERROR, "ESP32I2SDriver: RX initialization failed");
        shutdownTx();
        is_initialized_ = false;
        return false;
    }

    is_initialized_ = true;
    last_error_    = 0;
    Log::enqueue(Log::INFO, "ESP32I2SDriver initialized successfully");
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
    Log::enqueue(Log::INFO, "ESP32I2SDriver shut down");
}

bool ESP32I2SDriver::read(int32_t* buffer, std::size_t buffer_bytes, std::size_t& bytes_read,
                          uint32_t timeout_ms)
{
    if (!is_initialized_)
    {
        last_error_ = ESP_ERR_INVALID_STATE;
        bytes_read  = 0;
        return false;
    }

    if (!buffer || buffer_bytes == 0)
    {
        last_error_ = ESP_ERR_INVALID_ARG;
        bytes_read  = 0;
        return false;
    }

    // Perform blocking read from I2S RX
    esp_err_t ret = i2s_read(kI2SPortRx, buffer, buffer_bytes, &bytes_read, timeout_ms);

    if (ret != ESP_OK)
    {
        last_error_ = ret;
        return false;
    }

    last_error_ = 0;
    return true;
}

bool ESP32I2SDriver::write(const int32_t* buffer, std::size_t buffer_bytes,
                           std::size_t& bytes_written, uint32_t timeout_ms)
{
    if (!is_initialized_)
    {
        last_error_  = ESP_ERR_INVALID_STATE;
        bytes_written = 0;
        return false;
    }

    if (!buffer || buffer_bytes == 0)
    {
        last_error_   = ESP_ERR_INVALID_ARG;
        bytes_written = 0;
        return false;
    }

    // Perform blocking write to I2S TX
    // Note: i2s_write expects non-const pointer, so we cast away const here
    // The I2S driver does not modify the buffer
    esp_err_t ret = i2s_write(kI2SPortTx, const_cast<int32_t*>(buffer), buffer_bytes,
                              &bytes_written, timeout_ms);

    if (ret != ESP_OK)
    {
        last_error_ = ret;
        return false;
    }

    last_error_ = 0;
    return true;
}

bool ESP32I2SDriver::reset()
{
    if (!is_initialized_)
    {
        return false;
    }

    // Clear error flag
    last_error_ = 0;

    // Perform I2S reset (if supported by driver)
    // For now, just clear error state
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
