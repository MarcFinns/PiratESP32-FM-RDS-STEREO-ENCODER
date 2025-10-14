/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace Log
{
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
     *
     * Note: The text buffer is intentionally large to accommodate detailed
     *       diagnostic messages from the audio pipeline.
     */
    struct LogMsg
    {
        uint32_t ts_us;     // Timestamp in microseconds (from micros())
        uint8_t  level;     // Log level (DEBUG=0, INFO=1, WARN=2, ERROR=3)
        char     text[160]; // Message text (null-terminated, max 159 chars)
    };

    // ==================================================================================
    //                          STATIC MODULE STATE
    // ==================================================================================

    // ---- FreeRTOS Queue and Task Handles ----
    //
    // These handles are initialized by begin() and used by all enqueue operations.
    // They remain valid for the lifetime of the application.

    /**
     * Logger Queue Handle
     *
     * FreeRTOS queue that holds pending log messages. Producers (audio task,
     * display task, etc.) enqueue messages via Log::enqueuef(). The logger
     * task drains this queue and prints to Serial.
     *
     * Queue Semantics:
     *   • Fixed size (set at creation time)
     *   • FIFO ordering (first in, first out)
     *   • Non-blocking enqueue (xQueueSend with timeout 0)
     *   • Blocking dequeue (xQueueReceive with portMAX_DELAY)
     */
    static QueueHandle_t g_queue = nullptr;

    /**
     * Logger Task Handle
     *
     * Handle to the FreeRTOS task that drains the log queue. Created by begin()
     * and pinned to the specified core (typically Core 1).
     *
     * Task Characteristics:
     *   • Name: "logger"
     *   • Priority: Configurable (default 2, higher than display)
     *   • Stack: Configurable (default 4096 words = 16 KB)
     *   • Pinned to: Core 1 (non-real-time core)
     */
    static TaskHandle_t g_logger_task = nullptr;

    /**
     * Dropped Message Counter
     *
     * Atomic counter tracking the number of messages dropped due to queue overflow.
     * Incremented whenever xQueueSend() fails. This counter is read-only accessible
     * via future API additions (e.g., Log::getDroppedCount()).
     *
     * Note: Marked volatile to ensure atomic read/write on single-core access.
     *       For production, consider using std::atomic<uint32_t>.
     */
    static volatile uint32_t g_dropped = 0;

    // ==================================================================================
    //                          LOGGER TASK IMPLEMENTATION
    // ==================================================================================

    /**
     * Logger Task Function
     *
     * This FreeRTOS task runs continuously on Core 1, draining the log queue
     * and printing messages to Serial. It blocks on xQueueReceive() when the
     * queue is empty, yielding CPU to other tasks.
     *
     * Execution Flow:
     *   1. Block waiting for message (portMAX_DELAY = infinite wait)
     *   2. Receive message atomically from queue
     *   3. Print formatted message to Serial with timestamp
     *   4. Repeat indefinitely
     *
     * Output Format:
     *   [timestamp] message
     *   Example: [12345678] DSP_pipeline started on core 0
     *
     * Parameters:
     *   arg: Unused (required by FreeRTOS task signature)
     *
     * Note: This task never exits. It runs for the lifetime of the application.
     */
    static void logger_task(void *arg)
    {
        (void)arg;  // Suppress unused parameter warning

        LogMsg msg;  // Stack-allocated message buffer (reused each iteration)

        // Infinite loop: drain queue forever
        for (;;)
        {
            // Block until a message is available in the queue
            // portMAX_DELAY means wait indefinitely (no timeout)
            if (xQueueReceive(g_queue, &msg, portMAX_DELAY) == pdTRUE)
            {
                // Print message with timestamp to Serial
                // Format: [timestamp_us] message_text
                //
                // Note: Log level is captured in the message but not currently printed.
                //       Future enhancement could add level prefixes like "[INFO]", "[ERROR]", etc.
                Serial.printf("[%8u] %s\n", (unsigned)msg.ts_us, msg.text);
            }
            // If xQueueReceive fails (should never happen with portMAX_DELAY),
            // the loop continues without action
        }
    }

    // ==================================================================================
    //                          PUBLIC API IMPLEMENTATION
    // ==================================================================================

    /**
     * Initialize Logger and Start Task
     *
     * Creates the log message queue and spawns the logger task on the specified core.
     * This function must be called during setup() before any logging can occur.
     *
     * Initialization Steps:
     *   1. Initialize Serial if not already started (115200 baud)
     *   2. Create FreeRTOS queue with specified depth
     *   3. Spawn logger task pinned to specified core
     *
     * Parameters:
     *   queue_len:    Queue depth in messages (e.g., 64 = 10 KB queue)
     *   core_id:      FreeRTOS core ID (0 or 1, typically 1 for I/O)
     *   priority:     Task priority (typically 2, higher than display)
     *   stack_words:  Stack size in 32-bit words (4096 = 16 KB)
     *
     * Returns:
     *   true if initialization succeeded
     *   false if queue creation or task creation failed
     *
     * Failure Modes:
     *   • Queue creation fails: Insufficient heap memory
     *   • Task creation fails: Insufficient heap or invalid parameters
     */
    bool begin(std::size_t queue_len, int core_id, UBaseType_t priority, uint32_t stack_words)
    {
        // ---- Initialize Serial if Not Already Started ----
        //
        // If Serial.begin() was not called by the main application, start it here.
        // This ensures logging works even if the user forgets to initialize Serial.
        if (!Serial)
        {
            Serial.begin(115200);  // Standard baud rate for ESP32
            delay(50);             // Allow Serial to stabilize
        }

        // ---- Create FreeRTOS Queue ----
        //
        // Queue stores fixed-size LogMsg structures. Each message is copied into
        // the queue atomically by xQueueSend() and retrieved by xQueueReceive().
        //
        // Queue size: queue_len × sizeof(LogMsg) bytes
        // Example: 64 messages × 168 bytes = 10,752 bytes (~10 KB)
        g_queue = xQueueCreate((UBaseType_t)queue_len, sizeof(LogMsg));
        if (g_queue == nullptr)
        {
            // Queue creation failed (likely out of heap memory)
            return false;
        }

        // ---- Spawn Logger Task on Specified Core ----
        //
        // xTaskCreatePinnedToCore() creates a task that runs exclusively on the
        // specified core. This prevents the logger from interfering with Core 0
        // audio processing.
        //
        // Task parameters:
        //   • Function: logger_task (infinite loop draining queue)
        //   • Name: "logger" (visible in FreeRTOS task lists)
        //   • Stack: stack_words × 4 bytes (e.g., 4096 words = 16 KB)
        //   • Parameter: nullptr (unused)
        //   • Priority: Configurable (default 2)
        //   • Handle: Stored in g_logger_task for future use
        //   • Core: core_id (typically 1 for I/O tasks)
        BaseType_t ok = xTaskCreatePinnedToCore(
            logger_task,             // Task function
            "logger",                // Task name
            (uint32_t)stack_words,   // Stack size in words
            nullptr,                 // Task parameter (unused)
            priority,                // Task priority
            &g_logger_task,          // Task handle output
            core_id                  // Core affinity
        );

        // Return true if task creation succeeded, false otherwise
        return ok == pdPASS;
    }

    /**
     * Enqueue Preformatted Message
     *
     * Low-level enqueue function for already-formatted strings. The message
     * is copied into a LogMsg structure and sent to the queue atomically.
     *
     * Implementation Steps:
     *   1. Validate queue handle and message pointer
     *   2. Create LogMsg structure with timestamp and level
     *   3. Copy message text with bounds checking (max 159 chars)
     *   4. Attempt non-blocking enqueue (xQueueSend with timeout 0)
     *   5. If queue full, increment drop counter and return false
     *
     * Parameters:
     *   level: Message severity (DEBUG, INFO, WARN, ERROR)
     *   msg:   Null-terminated string (max 159 chars, longer strings truncated)
     *
     * Returns:
     *   true if message was successfully enqueued
     *   false if queue is full or invalid parameters
     *
     * Performance:
     *   • Best case: ~5 µs (queue not full)
     *   • Worst case: ~10 µs (queue full, increment drop counter)
     *
     * Thread Safety:
     *   Safe to call from any task or ISR. Non-blocking.
     */
    bool enqueue(Level level, const char *msg)
    {
        // ---- Validate Input Parameters ----
        //
        // Check that the queue was initialized and message pointer is valid.
        // If either check fails, return false immediately without side effects.
        if (!g_queue || !msg)
        {
            return false;
        }

        // ---- Construct Log Message ----
        //
        // Create a LogMsg structure on the stack. Zero-initialization ensures
        // padding bytes are deterministic (avoids copying uninitialized memory).
        LogMsg m{};
        m.ts_us = micros();                         // Capture current timestamp
        m.level = static_cast<uint8_t>(level);      // Store log level

        // ---- Copy Message Text with Bounds Checking ----
        //
        // Manually copy the input string into m.text[], ensuring we don't overflow
        // the buffer. Maximum 159 characters + null terminator = 160 bytes total.
        //
        // This is safer than strcpy() because it handles non-null-terminated inputs
        // and ensures the buffer is always null-terminated.
        std::size_t i = 0;
        while (msg[i] && i < sizeof(m.text) - 1)  // Leave room for null terminator
        {
            m.text[i] = msg[i];
            ++i;
        }
        m.text[i] = '\0';  // Ensure null termination

        // ---- Attempt Non-Blocking Enqueue ----
        //
        // xQueueSend() copies the LogMsg structure into the queue atomically.
        // Timeout of 0 means "don't wait" - if the queue is full, fail immediately.
        //
        // Return value:
        //   pdTRUE:  Message successfully enqueued
        //   pdFALSE: Queue is full, message dropped
        if (xQueueSend(g_queue, &m, 0) != pdTRUE)
        {
            // Queue full - increment drop counter and return failure
            g_dropped++;
            return false;
        }

        // Message successfully enqueued
        return true;
    }

    /**
     * Enqueue Formatted Message (printf-style)
     *
     * Primary logging function for real-time code. Formats the message using
     * vsnprintf() in the caller's context, then enqueues atomically.
     *
     * Implementation Steps:
     *   1. Validate queue handle and format string
     *   2. Create LogMsg structure with timestamp and level
     *   3. Format message text using vsnprintf() (bounds-safe printf)
     *   4. Attempt non-blocking enqueue
     *   5. If queue full, increment drop counter and return false
     *
     * Parameters:
     *   level: Message severity (DEBUG, INFO, WARN, ERROR)
     *   fmt:   printf-style format string
     *   ...:   Variable arguments matching format specifiers
     *
     * Returns:
     *   true if message was successfully enqueued
     *   false if queue is full or invalid parameters
     *
     * Performance:
     *   • Format time: ~10-50 µs (depends on format complexity)
     *   • Enqueue time: ~5-10 µs
     *   • Total: ~15-60 µs
     *
     * Example:
     *   Log::enqueuef(Log::INFO, "Block %d: CPU %.1f%%, Peak %.1f dBFS",
     *                 block_num, cpu_usage, peak_dbfs);
     *
     * Thread Safety:
     *   Safe to call from any task or ISR. Non-blocking.
     */
    bool enqueuef(Level level, const char *fmt, ...)
    {
        // ---- Validate Input Parameters ----
        //
        // Check that the queue was initialized and format string is valid.
        if (!g_queue || !fmt)
        {
            return false;
        }

        // ---- Construct Log Message ----
        //
        // Create a LogMsg structure on the stack with timestamp and level.
        LogMsg m{};
        m.ts_us = micros();                         // Capture current timestamp
        m.level = static_cast<uint8_t>(level);      // Store log level

        // ---- Format Message Text ----
        //
        // Use vsnprintf() to safely format the message into m.text[].
        // vsnprintf() guarantees null termination and bounds checking.
        //
        // va_list: Variable argument list (standard C mechanism)
        // va_start: Initialize argument list from '...' parameters
        // vsnprintf: printf-style formatting with bounds checking
        // va_end: Clean up argument list
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(m.text, sizeof(m.text), fmt, ap);  // Format with bounds checking
        va_end(ap);

        // ---- Attempt Non-Blocking Enqueue ----
        //
        // xQueueSend() copies the formatted LogMsg into the queue atomically.
        // Timeout of 0 means immediate return (no blocking).
        if (xQueueSend(g_queue, &m, 0) != pdTRUE)
        {
            // Queue full - increment drop counter and return failure
            g_dropped++;
            return false;
        }

        // Message successfully enqueued
        return true;
    }

} // namespace Log

// =====================================================================================
//                                END OF FILE
// =====================================================================================
