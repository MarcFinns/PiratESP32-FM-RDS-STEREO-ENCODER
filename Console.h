/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                        Console Interface
 *
 * =====================================================================================
 *
 * File:         Console.h
 * Description:  Serial console (CLI) + non-blocking log draining for RT audio
 *
 * Architecture:
 *   The console runs on a separate FreeRTOS task (typically Core 1) to prevent
 *   Serial I/O blocking from interfering with real-time audio processing on
 *   Core 0. Messages are enqueued via a lock-free FreeRTOS queue and drained
 *   asynchronously by the console task.
 *
 *   The Console class inherits from ModuleBase and follows the standardized task
 *   lifecycle pattern for consistency with other system modules.
 *
 * Key Features:
 *   * Lock-free queueing: Audio thread never blocks on Serial I/O
 *   * Fixed-size messages: No dynamic memory allocation in RT path
 *   * Drop-on-overflow: If queue is full, messages are dropped with counter
 *   * Timestamped: Each message includes microsecond timestamp
 *   * Formatted output: printf-style formatting via enqueuef()
 *   * ModuleBase compliance: Unified lifecycle management (begin/process/shutdown)
 *
 * Usage Pattern:
 *   1. Call Console::startTask() during setup (runs on Core 1)
 *   2. From audio thread (Core 0), call Console::enqueuef() to send messages
 *   3. Console task drains queue and prints to Serial asynchronously
 *
 * Thread Safety:
 *   All public functions are safe to call from any task or ISR context.
 *   FreeRTOS queue provides atomic enqueue operations.
 *
 * =====================================================================================
 */

#pragma once

#include "ModuleBase.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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
enum class LogLevel : uint8_t
{
    DEBUG = 0, // Detailed debug information
    INFO = 1,  // General informational messages
    WARN = 2,  // Warning conditions
    ERROR = 3  // Error conditions
};

/**
 * Console - Serial Console + Log Drainer
 *
 * Thread-safe, non-blocking logging system for real-time audio applications.
 * Inherits from ModuleBase to provide unified task lifecycle management.
 *
 * The console task runs continuously on a dedicated FreeRTOS core (typically Core 1),
 * draining messages from a fixed-size queue and outputting them to Serial. Producers
 * (audio task, display task, etc.) enqueue messages non-blockingly, ensuring real-time
 * operations are never delayed by I/O.
 *
 * Queue Semantics (See QueueContracts.md for full design rationale):
 *   * Type: FIFO with drop-oldest-on-overflow
 *   * Size: Fixed at task creation time (default 64 messages = ~10 KB)
 *   * Behavior: Non-blocking send (xQueueSend with timeout 0)
 *     - If queue is full: oldest message silently dropped, counter incremented
 *     - Message never blocks sender, ever
 *   * Rationale: Logging is diagnostic/non-critical. Freshness more important than
 *     completeness. Each message has timestamp, so loss is detectable. Serial I/O
 *     latency means older messages already stale by time of print.
 *   * Tradeoff: Cannot guarantee all messages logged, but ensures real-time code
 *     never blocked by logger queue
 *
 * Performance:
 *   * Enqueue operation: ~5-10 uss
 *   * Non-blocking: Immediate return, never waits
 *   * Memory per message: 160 bytes (timestamp + level + 159-char string)
 */
class Console : public ModuleBase
{
  public:
    /**
     * Get Console Singleton Instance
     *
     * Returns reference to the single global Log instance.
     * This instance is created statically and manages the entire
     * logger subsystem.
     *
     * Returns:
     *   Reference to the singleton Log instance
     */
    static Console &getInstance();

    /**
     * Initialize Console System and Start Task
     *
     * Creates a FreeRTOS queue and spawns the console task on the specified core.
     * The console task will drain the queue and output messages to Serial.
     *
     * Static wrapper for backward compatibility that delegates to the singleton.
     *
     * Parameters:
     *   queue_len:    Number of log messages the queue can hold (default: 64)
     *   core_id:      FreeRTOS core to pin the console task (default: 1)
     *   priority:     FreeRTOS task priority (default: 2)
     *   stack_words:  Task stack size in 32-bit words (default: 4096 = 16 KB)
     *
     * Returns:
     *   true if initialization successful, false on failure
     */
    static bool begin(size_t queue_len = 64, int core_id = 1, uint32_t priority = 2,
                      uint32_t stack_words = 4096);

    /**
     * Start Console Task (Convenience Wrapper)
     *
     * Alternative initialization function with parameter order matching
     * other system modules for consistency across the codebase.
     *
     * Static wrapper for backward compatibility.
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
     *   Console::startTask(1, 2, 4096, 128);
     */
    static bool startTask(int core_id, uint32_t priority, uint32_t stack_words,
                          size_t queue_len = 64);

    /**
     * Enqueue Formatted Log Message (printf-style)
     *
     * Sends a formatted message to the logger queue. This is the primary
     * logging function for real-time code paths. Formatting occurs in the
     * caller's context, then the message is enqueued atomically.
     *
     * Static wrapper for backward compatibility.
     *
     * Parameters:
     *   level: Message severity level (LogLevel enumeration)
     *   fmt:   printf-style format string
     *   ...:   Variable arguments matching format specifiers
     *
     * Returns:
     *   true if message was enqueued successfully
     *   false if queue is full (message dropped)
     *
     * Example:
     *   Console::enqueuef(LogLevel::INFO, "Audio block %d processed", block);
     */
  static bool enqueuef(LogLevel level, const char *fmt, ...);

    /**
     * Enqueue Preformatted Log Message
     *
     * Low-level function to enqueue an already-formatted string.
     *
     * Static wrapper for backward compatibility.
     *
     * Parameters:
     *   level: Message severity level (LogLevel enumeration)
     *   msg:   Preformatted null-terminated string (max 159 chars)
     *
     * Returns:
     *   true if message was enqueued successfully
     *   false if queue is full
     *
     * Example:
     *   Console::enqueue(LogLevel::WARN, "Custom message");
     */
    static bool enqueue(LogLevel level, const char *msg);

    /**
     * Print via logger if available, otherwise fall back to Serial.
     * Returns true if enqueued to logger, false if printed via Serial or failed.
     */
    static bool printOrSerial(LogLevel level, const char *msg);

  /**
   * Formatted variant: routes to logger if available, otherwise Serial.printf.
   * Returns true if enqueued to logger, false if printed via Serial or failed.
   */
    static bool printfOrSerial(LogLevel level, const char *fmt, ...);

    /** Quick filter to check if a given level should be logged right now.
     * Returns true during startup, or when not muted and level >= current threshold. */
    static bool shouldLog(LogLevel level);

    /**
     * Returns true once the console task has begun and is running
     * (i.e., queue is created and begin() completed).
     */
    static bool isReady();

    /**
     * Mark End of Startup Phase
     *
     * Called after "System Ready" message to indicate that startup initialization
     * is complete. When SYST:LOG:LEVEL is set to OFF, this allows the system to
     * show the full startup sequence but then suppress periodic logging.
     *
     * Should be called from DSP_pipeline::begin() after final initialization.
     */
    static void markStartupComplete();

  private:
    /**
     * Private Constructor (Singleton Pattern)
     *
     * Initializes module state. Only called once during getInstance().
     */
    Console();

    /**
     * Virtual Destructor Implementation
     */
    virtual ~Console() = default;

    /**
     * Initialize Module Resources (ModuleBase contract)
     *
     * Called once when the task starts. Initializes Serial communication
     * and prepares the logger for operation.
     *
     * Returns:
     *   true if initialization successful, false otherwise
     */
    bool begin() override;

    /**
     * Main Processing Loop Body (ModuleBase contract)
     *
     * Called repeatedly in infinite loop. Drains one message from the queue
     * and outputs it to Serial. If queue is empty, blocks waiting for message.
     */
    void process() override;

    /**
     * Shutdown Module Resources (ModuleBase contract)
     *
     * Called during graceful shutdown. Could flush remaining queue messages.
     */
    void shutdown() override;

    /**
     * Task Trampoline (FreeRTOS Entry Point)
     *
     * Static function provided to FreeRTOS task creation. Delegates to
     * ModuleBase::defaultTaskTrampoline() for standard lifecycle handling.
     *
     * Parameters:
     *   arg: Pointer to Log instance (passed from xTaskCreatePinnedToCore)
     */
    static void taskTrampoline(void *arg);

    /**
     * Instance Method - Enqueue Formatted Message
     *
     * Core implementation of enqueuef(). Formats message and attempts
     * non-blocking enqueue.
     *
     * Parameters:
     *   level: Message severity level
     *   fmt:   printf-style format string
     *   ap:    Variable argument list (already initialized)
     *
     * Returns:
     *   true if successfully enqueued, false if queue full
     */
  bool enqueueFormatted(LogLevel level, const char *fmt, va_list ap);

    /**
     * Instance Method - Enqueue Preformatted Message
     *
     * Core implementation of enqueue(). Attempts non-blocking queue insertion.
     *
     * Parameters:
     *   level: Message severity level
     *   msg:   Null-terminated message string
     *
     * Returns:
     *   true if successfully enqueued, false if queue full
     */
    bool enqueueRaw(LogLevel level, const char *msg);

    // ==================================================================================
    //                          MEMBER STATE VARIABLES
    // ==================================================================================

    /**
     * Console Queue Handle
     *
     * FreeRTOS queue for log messages. Created during begin() and destroyed
     * during shutdown().
     */
    QueueHandle_t queue_;

    /**
     * Configuration: Queue Depth
     *
     * Number of messages the queue can hold. Set during task creation.
     */
    size_t queue_len_;

    /**
     * Dropped Message Counter
     *
     * Incremented whenever a message cannot be enqueued due to queue overflow.
     * Marked volatile for atomic access patterns.
     */
    volatile uint32_t dropped_count_;

    /**
     * Configuration: Core ID
     *
     * FreeRTOS core (0 or 1) that the console task runs on. Typically 1.
     */
    int core_id_;

    /**
     * Configuration: Task Priority
     *
     * FreeRTOS task priority. Typically 2 (higher than display, lower than audio).
     */
    uint32_t priority_;

    /**
     * Configuration: Stack Size
     *
     * Task stack size in 32-bit words. Typically 4096 (16 KB).
     */
    uint32_t stack_words_;
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
