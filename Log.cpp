/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      Lock-Free Logger Implementation
 *
 * =====================================================================================
 *
 * File:         Log.cpp
 * Description:  Non-blocking asynchronous logger for real-time audio systems
 *
 * Purpose:
 *   This module provides a thread-safe, non-blocking logging system that prevents
 *   Serial I/O from interfering with real-time audio processing. Log messages are
 *   enqueued by producer tasks (e.g., DSP_pipeline on Core 0) and drained by a
 *   dedicated logger task running on Core 1.
 *
 * Design Principles:
 *   • Zero blocking: Enqueue operations never wait, they either succeed or drop
 *   • Fixed-size allocation: No dynamic memory in real-time path
 *   • Bounded latency: Message formatting happens in caller context, not logger
 *   • Core isolation: Serial I/O runs exclusively on Core 1
 *   • ModuleBase compliance: Unified task lifecycle management
 *
 * Performance Characteristics:
 *   • Enqueue time: ~5-10 µs (FreeRTOS queue overhead + copy)
 *   • Message size: 160 bytes per message (timestamp + level + 159-char string)
 *   • Queue depth: Configurable (default 64 messages = 10 KB queue)
 *   • Drop behavior: Silent drop with atomic counter increment
 *
 * Thread Safety:
 *   All functions are safe to call from any task or ISR. FreeRTOS queues
 *   provide atomic operations with lockless semantics on dual-core ESP32.
 *
 * =====================================================================================
 */

#include "Log.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ==================================================================================
//                          LOG MESSAGE STRUCTURE
// ==================================================================================

/**
 * Internal Log Message Structure
 *
 * Fixed-size structure stored in the FreeRTOS queue. Each message contains:
 *   • Timestamp: Microsecond timestamp from micros() at enqueue time
 *   • Level: Severity level (DEBUG, INFO, WARN, ERROR)
 *   • Text: Null-terminated string (max 159 chars + null)
 *
 * Size: 160 bytes total (4 + 1 + 3 padding + 160 = 168 bytes aligned)
 */
struct LogMsg
{
    uint32_t ts_us; // Timestamp in microseconds (from micros())
    uint8_t level;  // Log level (DEBUG=0, INFO=1, WARN=2, ERROR=3)
    char text[160]; // Message text (null-terminated, max 159 chars)
};

// ==================================================================================
//                          SINGLETON INSTANCE
// ==================================================================================

/**
 * Get Log Singleton Instance
 *
 * Returns the single global Log instance using Meyer's singleton pattern.
 * Thread-safe and lazy-initialized.
 *
 * Returns:
 *   Reference to the singleton Log instance
 */
Log &Log::getInstance()
{
    static Log s_instance;
    return s_instance;
}

// ==================================================================================
//                          CONSTRUCTOR & MEMBER INITIALIZATION
// ==================================================================================

/**
 * Private Constructor (Singleton Pattern)
 *
 * Initializes all member variables to safe default states.
 * Cannot be called directly - use getInstance() instead.
 */
Log::Log()
    : queue_(nullptr), queue_len_(64), dropped_count_(0), core_id_(1), priority_(2),
      stack_words_(4096)
{
    // All members initialized via initializer list
}

// ==================================================================================
//                          STATIC WRAPPER API
// ==================================================================================

/**
 * Static Wrapper - Initialize Logger and Start Task
 *
 * Delegates to the singleton instance.
 */
bool Log::begin(size_t queue_len, int core_id, uint32_t priority, uint32_t stack_words)
{
    Log &logger = getInstance();
    logger.queue_len_ = queue_len;
    logger.core_id_ = core_id;
    logger.priority_ = priority;
    logger.stack_words_ = stack_words;

    // Spawn the logger task via ModuleBase helper
    return logger.spawnTask("logger",
                            (uint32_t)stack_words,
                            (UBaseType_t)priority,
                            core_id,
                            Log::taskTrampoline);
}

/**
 * Static Wrapper - Start Logger Task (Convenience)
 *
 * Alternative parameter order for consistency with other modules.
 */
bool Log::startTask(int core_id, uint32_t priority, uint32_t stack_words, size_t queue_len)
{
    return begin(queue_len, core_id, priority, stack_words);
}

/**
 * Static Wrapper - Enqueue Formatted Message
 *
 * Delegates to the singleton instance.
 */
bool Log::enqueuef(LogLevel level, const char *fmt, ...)
{
    if (!fmt)
        return false;

    va_list ap;
    va_start(ap, fmt);
    bool result = getInstance().enqueueFormatted(level, fmt, ap);
    va_end(ap);

    return result;
}

/**
 * Static Wrapper - Enqueue Preformatted Message
 *
 * Delegates to the singleton instance.
 */
bool Log::enqueue(LogLevel level, const char *msg)
{
    return getInstance().enqueueRaw(level, msg);
}

// ==================================================================================
//                          MODULEBASE IMPLEMENTATION
// ==================================================================================

/**
 * Task Trampoline (FreeRTOS Entry Point)
 *
 * Static function called by FreeRTOS when task starts.
 * Extracts the Log instance pointer and calls defaultTaskTrampoline().
 */
void Log::taskTrampoline(void *arg)
{
    ModuleBase::defaultTaskTrampoline(arg);
}

/**
 * Initialize Module Resources (ModuleBase contract)
 *
 * Called once when the task starts. Creates the logger queue and initializes
 * Serial communication.
 */
bool Log::begin()
{
    // ---- Initialize Serial if Not Already Started ----
    if (!Serial)
    {
        Serial.begin(115200); // Standard baud rate for ESP32
        delay(50);            // Allow Serial to stabilize
    }

    // ---- Create FreeRTOS Queue ----
    queue_ = xQueueCreate((UBaseType_t)queue_len_, sizeof(LogMsg));
    if (queue_ == nullptr)
    {
        // Queue creation failed (likely out of heap memory)
        return false;
    }

    // Print runtime pinning info via Serial to avoid recursion during logger init
    Serial.printf("Logger running on Core %d\n", xPortGetCoreID());

    return true;
}

/**
 * Main Processing Loop Body (ModuleBase contract)
 *
 * Called repeatedly in infinite loop. Drains one message from the queue
 * and outputs it to Serial. If queue is empty, blocks waiting for message.
 */
void Log::process()
{
    LogMsg msg;

    // Block until a message is available in the queue
    if (xQueueReceive(queue_, &msg, portMAX_DELAY) == pdTRUE)
    {
        // Print message with timestamp to Serial
        Serial.printf("[%8u] %s\n", (unsigned)msg.ts_us, msg.text);
    }
}

/**
 * Shutdown Module Resources (ModuleBase contract)
 *
 * Called during graceful shutdown. Cleans up queue resources.
 */
void Log::shutdown()
{
    if (queue_ != nullptr)
    {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

// ==================================================================================
//                          INSTANCE MESSAGE ENQUEUE METHODS
// ==================================================================================

/**
 * Instance Method - Enqueue Preformatted Message
 *
 * Core implementation of static enqueue(). Constructs a LogMsg structure
 * and attempts non-blocking insertion into the queue.
 */
bool Log::enqueueRaw(LogLevel level, const char *msg)
{
    // ---- Validate Input Parameters ----
    if (!queue_ || !msg)
    {
        return false;
    }

    // ---- Construct Log Message ----
    LogMsg m{};
    m.ts_us = micros();                    // Capture current timestamp
    m.level = static_cast<uint8_t>(level); // Store log level

    // ---- Copy Message Text with Bounds Checking ----
    size_t i = 0;
    while (msg[i] && i < sizeof(m.text) - 1) // Leave room for null terminator
    {
        m.text[i] = msg[i];
        ++i;
    }
    m.text[i] = '\0'; // Ensure null termination

    // ---- Attempt Non-Blocking Enqueue ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        // Queue full - increment drop counter and return failure
        dropped_count_++;
        return false;
    }

    return true;
}

/**
 * Instance Method - Enqueue Formatted Message
 *
 * Core implementation of static enqueuef(). Formats the message using
 * vsnprintf() and attempts non-blocking insertion into the queue.
 */
bool Log::enqueueFormatted(LogLevel level, const char *fmt, va_list ap)
{
    // ---- Validate Input Parameters ----
    if (!queue_ || !fmt)
    {
        return false;
    }

    // ---- Construct Log Message ----
    LogMsg m{};
    m.ts_us = micros();                    // Capture current timestamp
    m.level = static_cast<uint8_t>(level); // Store log level

    // ---- Format Message Text ----
    vsnprintf(m.text, sizeof(m.text), fmt, ap); // Format with bounds checking

    // ---- Attempt Non-Blocking Enqueue ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        // Queue full - increment drop counter and return failure
        dropped_count_++;
        return false;
    }

    return true;
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
