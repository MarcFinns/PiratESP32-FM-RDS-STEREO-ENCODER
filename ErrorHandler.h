/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                        Error Handler Utility (Header Only)
 *
 * =====================================================================================
 *
 * File:         ErrorHandler.h
 * Description:  Standardized error handling and reporting across all modules
 *
 * Purpose:
 *   Provides consistent error codes, logging patterns, and recovery policies.
 *   All modules use these standard return codes and error logging patterns
 *   to ensure predictable error behavior and easier debugging.
 *
 * Design Principles:
 *   * Standard return codes for common failure modes
 *   * Non-blocking error logging via Log module (never blocks real-time code)
 *   * Clear distinction between silent failures and logged failures
 *   * Recovery strategies for transient failures (queue full, task creation, etc.)
 *
 * Error Categories:
 *   * INITIALIZATION: begin() failures, resource allocation
 *   * QUEUE: Overflow, underflow, not initialized
 *   * TASK: Task creation, scheduling failures
 *   * HARDWARE: I2S errors, device failures
 *   * PARAMETER: Invalid function arguments
 *   * TIMEOUT: Operations exceeding time limits
 *
 * Usage Pattern:
 *   if (!operation()) {
 *       ErrorHandler::logError(ErrorCode::QUEUE_FULL, "VUMeter::enqueue");
 *       return false;
 *   }
 *
 * =====================================================================================
 */

#pragma once

#include "Console.h"

#include <cstdint>

/**
 * ErrorCode - Standard Return Codes for All Modules
 *
 * All system modules use these codes to communicate operation status.
 * Provides consistent semantics across the entire system.
 */
enum class ErrorCode : uint8_t
{
    // Success
    OK = 0, // Operation completed successfully

    // Initialization Failures (Module begin())
    INIT_FAILED = 10,          // General initialization failure
    INIT_HARDWARE_FAILED = 11, // Hardware driver initialization failed
    INIT_QUEUE_FAILED = 12,    // Queue creation failed (out of memory)
    INIT_RESOURCE_FAILED = 13, // Resource allocation failed

    // Queue Failures
    QUEUE_FULL = 20,            // Queue overflow (message/sample dropped)
    QUEUE_EMPTY = 21,           // Queue underflow (no data available)
    QUEUE_NOT_INITIALIZED = 22, // Queue handle is nullptr
    QUEUE_SEND_FAILED = 23,     // xQueueSend() returned error

    // Task Failures
    TASK_CREATE_FAILED = 30, // xTaskCreatePinnedToCore() failed
    TASK_NOT_RUNNING = 31,   // Module task not initialized
    TASK_DELETE_FAILED = 32, // Task deletion failed

    // Hardware Failures
    HARDWARE_ERROR = 40,      // General hardware error
    I2S_READ_ERROR = 41,      // I2S RX operation failed
    I2S_WRITE_ERROR = 42,     // I2S TX operation failed
    I2S_NOT_INITIALIZED = 43, // I2S interface not ready

    // Parameter Validation
    INVALID_PARAM = 50,   // Invalid function parameter
    INVALID_POINTER = 51, // Null pointer where required
    INVALID_RANGE = 52,   // Parameter out of valid range
    INVALID_STATE = 53,   // Operation not valid in current state

    // Timeout
    TIMEOUT = 60,            // Operation exceeded time limit
    DEADLOCK_SUSPECTED = 61, // Possible deadlock detected

    // Data Errors
    CHECKSUM_ERROR = 70, // CRC or checksum mismatch
    DATA_CORRUPT = 71,   // Data corruption detected
    UNDERRUN = 72,       // Audio underrun (insufficient data)
    OVERRUN = 73,        // Audio overrun (buffer overflow)

    // System Errors
    OUT_OF_MEMORY = 80,  // Heap allocation failed
    STACK_OVERFLOW = 81, // Stack size insufficient
    SYSTEM_ERROR = 82,   // Unclassified system error

    // Unknown
    UNKNOWN = 255 // Unknown error code
};

/**
 * ErrorHandler - Static Error Handling Utility
 *
 * Provides consistent error handling patterns, logging, and recovery strategies
 * across all system modules. All functions are static and header-only for
 * zero-overhead abstraction.
 *
 * Design:
 *   - logError(): Log error with module context
 *   - logWarning(): Log warning-level message
 *   - logInfo(): Log informational message
 *   - isRecoverable(): Determine if error allows graceful recovery
 *   - shouldRetry(): Determine if operation should be retried
 */
namespace ErrorHandler
{
/**
 * Convert ErrorCode to Human-Readable String (Forward Declaration)
 *
 * Helper function for logging and debugging.
 */
inline const char *errorCodeToString(ErrorCode error);

/**
 * Log Error with Module Context
 *
 * Non-blocking error logging function. Enqueues error message via Log module
 * without blocking real-time code paths.
 *
 * Parameters:
 *   error:    ErrorCode enumeration value
 *   context:  Module/function name (e.g., "VUMeter::enqueue")
 *   details:  Optional additional information (e.g., "queue is full")
 *
 * Usage:
 *   ErrorHandler::logError(ErrorCode::QUEUE_FULL, "VUMeter::enqueue", "overwrite failed");
 */
inline void logError(ErrorCode error, const char *context, const char *details = nullptr)
{
    const char *error_str = errorCodeToString(error);
    if (details)
    {
        Console::enqueuef(LogLevel::ERROR, "[%s] %s: %s", error_str, context, details);
    }
    else
    {
        Console::enqueuef(LogLevel::ERROR, "[%s] %s", error_str, context);
    }
}

/**
 * Log Warning with Module Context
 *
 * Non-blocking warning logging function.
 *
 * Parameters:
 *   context:  Module/function name
 *   message:  Warning message
 *
 * Usage:
 *   ErrorHandler::logWarning("DSP_pipeline", "CPU usage approaching limit");
 */
inline void logWarning(const char *context, const char *message)
{
    Console::enqueuef(LogLevel::WARN, "[%s] %s", context, message);
}

/**
 * Log Informational Message
 *
 * Non-blocking info logging function.
 *
 * Parameters:
 *   context:  Module/function name
 *   message:  Informational message
 *
 * Usage:
 *   ErrorHandler::logInfo("Log", "Module initialized successfully");
 */
inline void logInfo(const char *context, const char *message)
{
    Console::enqueuef(LogLevel::INFO, "[%s] %s", context, message);
}

/**
 * Check if Error is Recoverable
 *
 * Determines whether an error represents a transient failure that can be
 * recovered from or a fatal failure that requires intervention.
 *
 * Recoverable Errors:
 *   - QUEUE_FULL: Can retry later when queue drains
 *   - TIMEOUT: Can retry with longer timeout
 *   - UNDERRUN: Can recover with next block
 *
 * Fatal Errors:
 *   - INIT_FAILED: Cannot initialize, task must exit
 *   - HARDWARE_ERROR: Hardware malfunction
 *   - OUT_OF_MEMORY: System resource exhaustion
 *
 * Parameters:
 *   error: ErrorCode to check
 *
 * Returns:
 *   true if error is transient and recovery is possible
 *   false if error is fatal
 */
inline bool isRecoverable(ErrorCode error)
{
    switch (error)
    {
    // Transient errors
    case ErrorCode::QUEUE_FULL:
    case ErrorCode::QUEUE_EMPTY:
    case ErrorCode::TIMEOUT:
    case ErrorCode::UNDERRUN:
    case ErrorCode::OVERRUN:
        return true;

    // Fatal errors
    case ErrorCode::INIT_FAILED:
    case ErrorCode::INIT_HARDWARE_FAILED:
    case ErrorCode::INIT_QUEUE_FAILED:
    case ErrorCode::TASK_CREATE_FAILED:
    case ErrorCode::HARDWARE_ERROR:
    case ErrorCode::OUT_OF_MEMORY:
    case ErrorCode::STACK_OVERFLOW:
    case ErrorCode::DATA_CORRUPT:
        return false;

    default:
        return false; // Unknown errors treated as fatal
    }
}

/**
 * Check if Operation Should be Retried
 *
 * Determines if a failed operation should be immediately retried
 * or if retry should be deferred/abandoned.
 *
 * Immediate Retry:
 *   - QUEUE_FULL: Retry after next cycle
 *   - TIMEOUT: Retry with adjusted timeout
 *
 * No Retry:
 *   - INVALID_PARAM: Logic error, won't fix without code change
 *   - INIT_FAILED: Already logged, can't retry initialization
 *   - HARDWARE_ERROR: Persistent problem
 *
 * Parameters:
 *   error: ErrorCode to evaluate
 *
 * Returns:
 *   true if operation should be retried
 *   false if retry is futile
 */
inline bool shouldRetry(ErrorCode error)
{
    switch (error)
    {
    case ErrorCode::QUEUE_FULL:
    case ErrorCode::QUEUE_EMPTY:
    case ErrorCode::TIMEOUT:
    case ErrorCode::UNDERRUN:
    case ErrorCode::OVERRUN:
        return true;

    case ErrorCode::INVALID_PARAM:
    case ErrorCode::INVALID_POINTER:
    case ErrorCode::INVALID_RANGE:
    case ErrorCode::INVALID_STATE:
    case ErrorCode::INIT_FAILED:
    case ErrorCode::HARDWARE_ERROR:
    case ErrorCode::OUT_OF_MEMORY:
        return false;

    default:
        return false;
    }
}

/**
 * Convert ErrorCode to Human-Readable String
 *
 * Helper function for logging and debugging.
 *
 * Parameters:
 *   error: ErrorCode enumeration value
 *
 * Returns:
 *   Pointer to static string describing the error
 */
inline const char *errorCodeToString(ErrorCode error)
{
    switch (error)
    {
    case ErrorCode::OK:
        return "OK";
    case ErrorCode::INIT_FAILED:
        return "INIT_FAILED";
    case ErrorCode::INIT_HARDWARE_FAILED:
        return "INIT_HARDWARE_FAILED";
    case ErrorCode::INIT_QUEUE_FAILED:
        return "INIT_QUEUE_FAILED";
    case ErrorCode::INIT_RESOURCE_FAILED:
        return "INIT_RESOURCE_FAILED";
    case ErrorCode::QUEUE_FULL:
        return "QUEUE_FULL";
    case ErrorCode::QUEUE_EMPTY:
        return "QUEUE_EMPTY";
    case ErrorCode::QUEUE_NOT_INITIALIZED:
        return "QUEUE_NOT_INITIALIZED";
    case ErrorCode::QUEUE_SEND_FAILED:
        return "QUEUE_SEND_FAILED";
    case ErrorCode::TASK_CREATE_FAILED:
        return "TASK_CREATE_FAILED";
    case ErrorCode::TASK_NOT_RUNNING:
        return "TASK_NOT_RUNNING";
    case ErrorCode::TASK_DELETE_FAILED:
        return "TASK_DELETE_FAILED";
    case ErrorCode::HARDWARE_ERROR:
        return "HARDWARE_ERROR";
    case ErrorCode::I2S_READ_ERROR:
        return "I2S_READ_ERROR";
    case ErrorCode::I2S_WRITE_ERROR:
        return "I2S_WRITE_ERROR";
    case ErrorCode::I2S_NOT_INITIALIZED:
        return "I2S_NOT_INITIALIZED";
    case ErrorCode::INVALID_PARAM:
        return "INVALID_PARAM";
    case ErrorCode::INVALID_POINTER:
        return "INVALID_POINTER";
    case ErrorCode::INVALID_RANGE:
        return "INVALID_RANGE";
    case ErrorCode::INVALID_STATE:
        return "INVALID_STATE";
    case ErrorCode::TIMEOUT:
        return "TIMEOUT";
    case ErrorCode::DEADLOCK_SUSPECTED:
        return "DEADLOCK_SUSPECTED";
    case ErrorCode::CHECKSUM_ERROR:
        return "CHECKSUM_ERROR";
    case ErrorCode::DATA_CORRUPT:
        return "DATA_CORRUPT";
    case ErrorCode::UNDERRUN:
        return "UNDERRUN";
    case ErrorCode::OVERRUN:
        return "OVERRUN";
    case ErrorCode::OUT_OF_MEMORY:
        return "OUT_OF_MEMORY";
    case ErrorCode::STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case ErrorCode::SYSTEM_ERROR:
        return "SYSTEM_ERROR";
    default:
        return "UNKNOWN";
    }
}
} // namespace ErrorHandler

// =====================================================================================
//                                END OF FILE
// =====================================================================================
