/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                           Module Base Class Template
 *
 * =====================================================================================
 *
 * File:         TaskBaseClass.h
 * Description:  Abstract base class for all FreeRTOS task modules
 *
 * Purpose:
 *   Provides a unified pattern for all system modules that run as FreeRTOS tasks.
 *   Encapsulates the boilerplate task creation, lifecycle management, and defines
 *   the contract that all modules must fulfill.
 *
 * Design Pattern:
 *   Template Method Pattern - defines the skeleton of task lifecycle in base class,
 *   letting subclasses implement specific steps (begin, process, shutdown).
 *
 * Module Lifecycle:
 *   1. Module instance created (constructor)
 *   2. startTask() called -> spawns FreeRTOS task
 *   3. Task entry -> begin() called for initialization
 *   4. Infinite loop: process() called repeatedly
 *   5. On error: begin() returns false -> task self-deletes
 *   6. On shutdown: stopTask() signals graceful termination
 *
 * Usage Example: Inherit from TaskBaseClass and implement begin(), process(), and shutdown()
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * TaskBaseClass - Abstract Base Class for Task-Based Modules
 *
 * All FreeRTOS task modules in the system inherit from this class.
 * Provides unified interface for task lifecycle management.
 *
 * Thread Safety:
 *   - Each module runs on its own FreeRTOS task (dedicated task, not thread-safe)
 *   - No shared state between modules (communication via queues only)
 *   - startTask() should only be called from main thread during setup()
 *   - Query methods (isRunning, getTaskHandle) are read-only and thread-safe
 *   - process() and begin() methods are NOT thread-safe (single-task only)
 *   - defaultTaskTrampoline() runs in the context of the module's own task
 *
 * Queue Communication Pattern:
 *   Modules communicate with each other exclusively via FreeRTOS queues:
 *   - No direct access to another module's data (task-local state)
 *   - Queue operations are atomic and thread-safe
 *   - Queue semantics (FIFO, mailbox, mailbox-drop) defined per module
 *   - Prevents race conditions and data corruption
 *
 * Subclass Requirements:
 *   1. Implement begin() - initialize module resources
 *   2. Implement process() - main work loop body
 *   3. Implement shutdown() - cleanup resources
 *   4. Create static taskTrampoline() - FreeRTOS entry point
 */
class TaskBaseClass
{
  public:
    /**
     * Static helper to spawn a FreeRTOS task for arbitrary objects (non-TaskBaseClass).
     * The caller is responsible for storing the returned TaskHandle_t.
     */
    static bool spawnTaskFor(void *self,
                             const char *task_name,
                             uint32_t stack_words,
                             UBaseType_t priority,
                             int core_id,
                             TaskFunction_t entry,
                             TaskHandle_t *out_handle)
    {
        TaskHandle_t handle = nullptr;
        TaskFunction_t fn = entry ? entry : TaskBaseClass::defaultTaskTrampoline;
        BaseType_t ok = xTaskCreatePinnedToCore(
            fn,
            task_name,
            stack_words,
            self,
            priority,
            &handle,
            core_id);
        if (ok == pdPASS)
        {
            if (out_handle)
                *out_handle = handle;
            return true;
        }
        return false;
    }
    /**
     * Virtual Destructor
     *
     * Ensures proper cleanup of derived class resources.
     */
    virtual ~TaskBaseClass() = default;

    /**
     * Initialize Module Resources
     *
     * Called once when the task starts, before entering the main loop.
     * Use this to:
     *   - Initialize hardware interfaces
     *   - Create queues or synchronization primitives
     *   - Verify preconditions
     *   - Load configuration
     *
     * Returns:
     *   true  - Initialization successful, proceed to process() loop
     *   false - Initialization failed, task will self-delete
     *
     * Note: Must be safe to call multiple times if restarts are needed
     */
    virtual bool begin() = 0;

    /**
     * Main Processing Loop Body
     *
     * Called repeatedly in an infinite loop after begin() succeeds.
     * Should complete as quickly as possible to maintain task responsiveness.
     *
     * Example implementations:
     *   - Log: Read from queue, format, output to serial
     *   - VUMeter: Read audio sample from queue, update display
     *   - RDSAssembler: Generate next RDS group, queue bits
     *   - DSP: Read audio block, process, write output
     *
     * Guidelines:
     *   - Keep processing time deterministic and minimal
     *   - Use blocking queue operations for synchronization
     *   - Don't allocate memory dynamically
     *   - Log errors via queue-based logging (if available)
     */
    virtual void process() = 0;

    /**
     * Shutdown Module Resources
     *
     * Called when module is being shut down gracefully.
     * Use this to:
     *   - Flush queues
     *   - Save state
     *   - Release resources
     *   - Stop hardware
     *
     * Note: May not be called in error scenarios - design defensively
     */
    virtual void shutdown() {}

    /**
     * Check if Module is Running
     *
     * Returns:
     *   true if module successfully initialized and running
     *   false if module failed to initialize or is shut down
     */
    bool isRunning() const
    {
        return is_running_;
    }

    /**
     * Get Task Handle
     *
     * Returns FreeRTOS task handle for this module.
     * Useful for task management and monitoring.
     *
     * Returns:
     *   TaskHandle_t - Handle to FreeRTOS task, or nullptr if not started
     */
    TaskHandle_t getTaskHandle() const
    {
        return task_handle_;
    }

  protected:
    /**
     * Protected Constructor
     *
     * Only called by derived classes. Initializes module state.
     */
    TaskBaseClass() : task_handle_(nullptr), is_running_(false) {}

    /**
     * Default Task Trampoline Implementation
     *
     * Can be used directly in derived classes if no special initialization needed:
     *
     * Example:
     *   class MyModule : public TaskBaseClass {
     *   private:
     *       static void taskTrampoline(void* arg) {
     *           TaskBaseClass::defaultTaskTrampoline(arg);
     *       }
     *   };
     *
     * Parameters:
     *   arg: Pointer to TaskBaseClass instance (passed from xTaskCreatePinnedToCore)
     *
     * Execution Flow:
     *   1. Cast void* to TaskBaseClass*
     *   2. Call begin() - if fails, task deletes itself
     *   3. Call process() in infinite loop
     *   4. Never returns (task runs until shutdown)
     */
    static void defaultTaskTrampoline(void *arg)
    {
        auto *self = static_cast<TaskBaseClass *>(arg);

        // Initialize module
        if (!self->begin())
        {
            // Initialization failed - task exits
            self->is_running_ = false;
            vTaskDelete(nullptr);
            return;
        }

        self->is_running_ = true;

        // Main infinite loop
        for (;;)
        {
            self->process();
        }
    }

    /**
     * Spawn a FreeRTOS task for this module with standardized setup.
     * Creates the task using the provided entry trampoline (or the default) and
     * stores the resulting TaskHandle_t via setTaskHandle().
     *
     * Parameters:
     *   task_name:   Name for the task (for diagnostics)
     *   stack_words: Stack size in 32-bit words
     *   priority:    FreeRTOS task priority
     *   core_id:     CPU core to pin the task
     *   entry:       Optional entry function (defaults to defaultTaskTrampoline)
     *
     * Returns:
     *   true if the task was created successfully; false otherwise
     */
    bool spawnTask(const char *task_name,
                   uint32_t stack_words,
                   UBaseType_t priority,
                   int core_id,
                   TaskFunction_t entry = nullptr)
    {
        TaskHandle_t handle = nullptr;
        TaskFunction_t fn = entry ? entry : TaskBaseClass::defaultTaskTrampoline;
        BaseType_t ok = xTaskCreatePinnedToCore(
            fn,
            task_name,
            stack_words,
            this,
            priority,
            &handle,
            core_id);
        if (ok == pdPASS)
        {
            setTaskHandle(handle);
            return true;
        }
        return false;
    }

    /**
     * Set Task Handle
     *
     * Called internally when task is created.
     * Stores FreeRTOS task handle for reference.
     *
     * Parameters:
     *   handle: FreeRTOS task handle from xTaskCreatePinnedToCore
     */
    void setTaskHandle(TaskHandle_t handle)
    {
        task_handle_ = handle;
    }

  private:
    TaskHandle_t task_handle_; ///< FreeRTOS task handle
    bool is_running_;          ///< true if module successfully initialized
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
