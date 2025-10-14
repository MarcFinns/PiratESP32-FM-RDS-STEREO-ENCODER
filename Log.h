/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                        Lock-Free Logger Interface
 *
 * =====================================================================================
 *
 * File:         Log.h
 * Description:  Non-blocking logging system for real-time audio applications
 *
 * Architecture:
 *   The logger runs on a separate FreeRTOS task (typically Core 1) to prevent
 *   Serial I/O blocking from interfering with real-time audio processing on
 *   Core 0. Messages are enqueued via a lock-free FreeRTOS queue and drained
 *   asynchronously by the logger task.
 *
 * Key Features:
 *   • Lock-free queueing: Audio thread never blocks on Serial I/O
 *   • Fixed-size messages: No dynamic memory allocation in RT path
 *   • Drop-on-overflow: If queue is full, messages are dropped with counter
 *   • Timestamped: Each message includes microsecond timestamp
 *   • Formatted output: printf-style formatting via enqueuef()
 *
 * Usage Pattern:
 *   1. Call Log::startTask() during setup (runs on Core 1)
 *   2. From audio thread (Core 0), call Log::enqueuef() to send messages
 *   3. Logger task drains queue and prints to Serial asynchronously
 *
 * Thread Safety:
 *   All public functions are safe to call from any task or ISR context.
 *   FreeRTOS queue provides atomic enqueue operations.
 *
 * =====================================================================================
 */

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>

namespace Log
{
    // ==================================================================================
    //                              LOG LEVEL DEFINITIONS
    // ==================================================================================

    /**
     * Log Level Enumeration
     *
     * Defines message severity levels. Currently used for filtering
     * or future extensibility (e.g., colored output, log level filtering).
     *
     * Levels (in order of increasing severity):
     *   DEBUG = 0: Verbose diagnostic information
     *   INFO  = 1: Normal operational messages
     *   WARN  = 2: Warning conditions (non-critical)
     *   ERROR = 3: Error conditions (requires attention)
     */
    enum Level : uint8_t
    {
        DEBUG = 0,  // Detailed debug information
        INFO  = 1,  // General informational messages
        WARN  = 2,  // Warning conditions
        ERROR = 3   // Error conditions
    };

    // ==================================================================================
    //                          LOGGER TASK INITIALIZATION
    // ==================================================================================

    /**
     * Initialize Logger System and Start Task
     *
     * Creates a FreeRTOS queue and spawns the logger task on the specified core.
     * The logger task will drain the queue and output messages to Serial.
     *
     * Parameters:
     *   queue_len:    Number of log messages the queue can hold (default: 64)
     *                 Larger queues reduce message drops under heavy logging
     *
     *   core_id:      FreeRTOS core to pin the logger task (default: 1)
     *                 Core 1 is recommended to keep I/O off the audio core
     *
     *   priority:     FreeRTOS task priority (default: 2)
     *                 Higher than display (1), lower than audio (6)
     *
     *   stack_words:  Task stack size in 32-bit words (default: 4096 = 16 KB)
     *                 Sufficient for printf formatting and Serial I/O
     *
     * Returns:
     *   true if initialization successful, false on failure
     *
     * Note: This function must be called before any logging can occur.
     *       If Serial is not initialized, it will be started at 115200 baud.
     */
    bool begin(std::size_t queue_len = 64, int core_id = 1,
               UBaseType_t priority = 2, uint32_t stack_words = 4096);

    /**
     * Start Logger Task (Convenience Wrapper)
     *
     * Alternative initialization function with parameter order matching
     * AudioEngine::startTask() for consistency across the codebase.
     *
     * Parameters:
     *   core_id:      FreeRTOS core to pin the task (typically 1)
     *   priority:     Task priority (typically 2)
     *   stack_words:  Stack size in words (typically 4096)
     *   queue_len:    Queue depth (default: 64 messages)
     *
     * Returns:
     *   true if initialization successful, false on failure
     *
     * Example:
     *   Log::startTask(1, 2, 4096, 128);  // Core 1, priority 2, 128-message queue
     */
    inline bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words,
                          std::size_t queue_len = 64)
    {
        return begin(queue_len, core_id, priority, stack_words);
    }

    // ==================================================================================
    //                          MESSAGE ENQUEUE FUNCTIONS
    // ==================================================================================

    /**
     * Enqueue Formatted Log Message (printf-style)
     *
     * Sends a formatted message to the logger queue. This is the primary
     * logging function for real-time code paths. Formatting occurs in the
     * caller's context, then the message is enqueued atomically.
     *
     * Parameters:
     *   level: Message severity level (DEBUG, INFO, WARN, ERROR)
     *   fmt:   printf-style format string
     *   ...:   Variable arguments matching format specifiers
     *
     * Returns:
     *   true if message was enqueued successfully
     *   false if queue is full (message dropped, drop counter incremented)
     *
     * Thread Safety:
     *   Safe to call from any task or ISR. Non-blocking (immediate return).
     *
     * Implementation Details:
     *   • Timestamp captured at call time (micros())
     *   • Message limited to 159 characters + null terminator
     *   • If queue is full, message is silently dropped
     *   • No dynamic memory allocation (stack-only formatting)
     *
     * Example:
     *   Log::enqueuef(Log::INFO, "Audio block %d processed in %d us", block, time);
     */
    bool enqueuef(Level level, const char *fmt, ...);

    /**
     * Enqueue Preformatted Log Message
     *
     * Low-level function to enqueue an already-formatted string.
     * Useful when the message is constructed elsewhere or when
     * avoiding printf overhead.
     *
     * Parameters:
     *   level: Message severity level (DEBUG, INFO, WARN, ERROR)
     *   msg:   Preformatted null-terminated string (max 159 chars)
     *
     * Returns:
     *   true if message was enqueued successfully
     *   false if queue is full (message dropped)
     *
     * Thread Safety:
     *   Safe to call from any task or ISR. Non-blocking.
     *
     * Note: The string is copied into the queue, so the caller
     *       does not need to keep the original string alive.
     *
     * Example:
     *   char buffer[100];
     *   sprintf(buffer, "Custom message");
     *   Log::enqueue(Log::WARN, buffer);
     */
    bool enqueue(Level level, const char *msg);

} // namespace Log

// =====================================================================================
//                                END OF FILE
// =====================================================================================
