/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                      System Context (IoC Container) Implementation
 *
 * =====================================================================================
 *
 * File:         SystemContext.cpp
 * Description:  Implementation of centralized dependency injection container
 *
 * Purpose:
 *   Manages the lifecycle and initialization/shutdown of all major system components.
 *   Enforces correct module startup order and dependency relationships.
 *
 * =====================================================================================
 */

#include "SystemContext.h"
#include "DSP_pipeline.h"
#include "ErrorHandler.h"
#include "IHardwareDriver.h"
#include "Console.h"
#include "RDSAssembler.h"
#include "DisplayManager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Arduino.h>
#include <esp_err.h>

// ==================================================================================
//                          SINGLETON INSTANCE
// ==================================================================================

/**
 * Get System Context Singleton Instance
 *
 * Thread-safe singleton accessor using Meyer's singleton pattern.
 * The static instance is created on first call and persists for the lifetime
 * of the application. This approach is thread-safe in C++ and avoids the
 * need for explicit locking.
 *
 * Returns:
 *   Reference to the SystemContext singleton
 */
SystemContext &SystemContext::getInstance()
{
    static SystemContext s_instance;
    return s_instance;
}

// ==================================================================================
//                          CONSTRUCTOR & DESTRUCTOR
// ==================================================================================

/**
 * Private Constructor (Singleton Pattern)
 *
 * Initializes all member variables to safe default states.
 * Cannot be called directly - use getInstance() instead.
 */
SystemContext::SystemContext()
    : hardware_driver_(nullptr), dsp_pipeline_(nullptr), is_initialized_(false), init_time_us_(0)
{
    // All members initialized via initializer list
}

/**
 * Private Destructor
 *
 * Prevents accidental deletion of singleton.
 * Calls shutdown() if system is still running.
 */
SystemContext::~SystemContext()
{
    if (is_initialized_)
    {
        shutdown();
    }
}

// ==================================================================================
//                          INITIALIZATION
// ==================================================================================

/**
 * Initialize All System Modules
 *
 * Starts all major system components in the strict order required for proper
 * operation:
 *
 * 1. Validate hardware driver (required)
 * 2. Initialize hardware driver (I2S setup, DMA, etc.)
 * 3. Start Console task (Core 1) - Serial owner; drains logs and handles CLI
 * 4. Start VU Meter task (Core 0) - display feedback
 * 5. Start RDS Assembler task (Core 1) if enabled - RDS bitstream generation
 * 6. Start DSP Pipeline task (Core 0) - audio processing (highest priority)
 *
 * This order ensures that:
 * - Hardware is ready before any DSP operations
 * - Logging is available for all modules
 * - Display feedback can show status
 * - Audio processing runs with highest priority
 *
 * Parameters:
 *   hardware_driver:  Injected hardware I/O driver (required)
 *   dsp_core_id:      Core assignment for DSP (default 0)
 *   dsp_priority:     DSP task priority (default 6, max is 25)
 *   dsp_stack_words:  DSP task stack size in 32-bit words (default 12288 = 48KB)
 *   enable_rds:       Whether to start RDS assembler module (default true)
 *
 * Returns:
 *   true  - All modules initialized successfully and running
 *   false - Initialization failed (check logs for details)
 *
 * Note:
 *   - Call exactly once during system startup
 *   - All subsequent method calls require is_initialized_ == true
 *   - If initialization fails, system is left in partial state (call shutdown())
 */
bool SystemContext::initialize(IHardwareDriver *hardware_driver, int dsp_core_id,
                               UBaseType_t dsp_priority, uint32_t dsp_stack_words, bool enable_rds)
{
    // Validate inputs
    if (hardware_driver == nullptr)
    {
        Console::enqueuef(LogLevel::ERROR, "SystemContext::initialize() - hardware_driver is nullptr");
        return false;
    }

    if (is_initialized_)
    {
        Console::enqueuef(LogLevel::WARN, "SystemContext::initialize() - already initialized");
        return false;
    }

    hardware_driver_ = hardware_driver;

    // ---- Step 1: Start Console Task (first) ----
    // Console must be started early so downstream modules can log
    // Priority 2: Medium, higher than VU meter and RDS
    if (!Console::startTask(Config::CONSOLE_CORE,        // core_id
                        Config::CONSOLE_PRIORITY,    // priority
                        Config::CONSOLE_STACK_WORDS, // stack_words
                        Config::CONSOLE_QUEUE_LEN))  // queue_len
    {
        Serial.println("Failed to start Console task");
        return false;
    }

    Console::enqueuef(LogLevel::INFO, "Console task started on Core %d", Config::CONSOLE_CORE);

    // Wait briefly for logger to become ready (queue created, begin() finished)
    {
        const int max_wait_ms = 200;
        int waited = 0;
        while (!Console::isReady() && waited < max_wait_ms)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited += 1;
        }
    }

    // ---- Step 2: Initialize Hardware Driver ----
    // This must happen before any DSP operations
    if (!hardware_driver_->initialize())
    {
        int  perr = hardware_driver_->getErrorStatus();
        const char* err_name = esp_err_to_name(static_cast<esp_err_t>(perr));

        // Map to top-level init failure and log via ErrorHandler with context
        ErrorHandler::logError(ErrorCode::INIT_HARDWARE_FAILED,
                               "SystemContext::initialize",
                               err_name);
        return false;
    }

    Console::enqueuef(LogLevel::INFO, "Hardware driver initialized");

    // ---- Step 3: Start VU Meter Task (Core 0) ----
    // Visual feedback for operator - lower priority than logging
    if (!DisplayManager::startTask(Config::VU_CORE,         // core_id
                                   Config::VU_PRIORITY,     // priority
                                   Config::VU_STACK_WORDS,  // stack_words
                                   Config::VU_QUEUE_LEN))   // queue_len (mailbox)
    {
        Console::enqueuef(LogLevel::WARN, "Failed to start DisplayManager task (non-critical)");
        // Non-critical failure - continue initialization
    }
    else
    {
        Console::enqueuef(LogLevel::INFO, "Display Manager task started on Core %d", Config::VU_CORE);
    }

    // ---- Step 4: Start RDS Assembler Task (Core 1) if Enabled ----
    if (enable_rds)
    {
        if (!RDSAssembler::startTask(Config::RDS_CORE,         // core_id
                                     Config::RDS_PRIORITY,     // priority
                                     Config::RDS_STACK_WORDS,  // stack_words
                                     Config::RDS_BIT_QUEUE_LEN)) // queue_len: bits
        {
            Console::enqueuef(LogLevel::WARN, "Failed to start RDSAssembler task (non-critical)");
            // Non-critical failure - continue without RDS
        }
        else
        {
            Console::enqueuef(LogLevel::INFO, "RDS Assembler task started on Core %d", Config::RDS_CORE);
        }
    }

    // ---- Step 4.5: Load Last Configuration (or defaults) ----
    // Load the last saved configuration from NVS if available
    // Otherwise use factory defaults with PS="PiratESP" and RT="Hello from ESP32 FM Stereo RDS encoder!"
    extern void Console_LoadLastConfiguration();
    Console_LoadLastConfiguration();

    // ---- Step 5: Start DSP Pipeline Task (Core 0) ----
    // CRITICAL: This must start last and run on Core 0 with highest priority
    // Audio processing cannot be interrupted. Use the same instance we store so
    // SystemContext::getDSPPipeline() returns the running object.
    dsp_pipeline_ = new DSP_pipeline(hardware_driver_);
    if (!dsp_pipeline_->startTaskInstance(
            dsp_core_id,      // core_id: Core 0 (dedicated audio processing)
            dsp_priority,     // priority: Highest (typically 6 for real-time audio)
            dsp_stack_words)) // stack_words: 12KB typical
    {
        Console::enqueuef(LogLevel::ERROR, "Failed to start DSP Pipeline task");
        delete dsp_pipeline_;
        dsp_pipeline_ = nullptr;
        return false;
    }

    Console::enqueuef(LogLevel::INFO, "DSP Pipeline task started on Core %d with priority %d",
                  dsp_core_id, dsp_priority);

    // Wait briefly for all tasks to emit their startup messages
    // This ensures DisplayManager, RDSAssembler, and DSP_pipeline can log before startup phase ends
    vTaskDelay(pdMS_TO_TICKS(500));

    // Signal that startup phase is complete
    // After this, if LOG_LEVEL=OFF, periodic logs will be suppressed
    Console::markStartupComplete();

    // ---- Initialization Complete ----
    is_initialized_ = true;
    init_time_us_ = esp_timer_get_time();

    Console::enqueuef(LogLevel::INFO, "SystemContext initialized - all modules running");

    return true;
}

// ==================================================================================
//                          SHUTDOWN
// ==================================================================================

/**
 * Shutdown All System Modules
 *
 * Cleanly shuts down all system components in reverse order of initialization:
 * 1. Stop DSP Pipeline (Core 0) - high priority, must stop first
 * 2. Stop RDS Assembler (Core 1)
 * 3. Stop VU Meter (Core 1)
 * 4. Stop Console (Core 1) - must stop last so we can log shutdown progress
 * 5. Shutdown hardware driver
 *
 * This reverse order ensures:
 * - Audio processing stops before anything else
 * - I/O tasks can clean up gracefully
 * - Logging is available during most of shutdown
 *
 * Note:
 *   - Safe to call even if initialization failed
 *   - Can be called from interrupt context with caution
 *   - After shutdown(), initialize() can be called again if needed
 */
void SystemContext::shutdown()
{
    if (!is_initialized_)
    {
        return; // Already shutdown or never initialized
    }

    Console::enqueuef(LogLevel::INFO, "SystemContext shutdown initiated");

    // ---- Step 1: Stop DSP Pipeline (Core 0) ----
    if (dsp_pipeline_ != nullptr)
    {
        // Note: DSP_pipeline needs a stop() method to signal graceful task shutdown.
        // For now, we delete the instance (task will be cleaned up by FreeRTOS).
        delete dsp_pipeline_;
        dsp_pipeline_ = nullptr;
        Console::enqueuef(LogLevel::INFO, "DSP Pipeline stopped");
    }

    // ---- Step 2: Stop RDS Assembler (Core 1) ----
    RDSAssembler::stopTask();
    Console::enqueuef(LogLevel::INFO, "RDS Assembler stopped");

    // ---- Step 3: Stop VU Meter (Core 1) ----
    DisplayManager::stopTask();
    Console::enqueuef(LogLevel::INFO, "Display Manager stopped");

    // ---- Step 4: Stop Console (Core 1) - Last ----
    Console::enqueuef(LogLevel::INFO, "SystemContext shutdown complete");
    Console::stopTask();

    // ---- Step 5: Shutdown Hardware Driver ----
    if (hardware_driver_ != nullptr)
    {
        hardware_driver_->shutdown();
        hardware_driver_ = nullptr;
    }

    is_initialized_ = false;
}

// ==================================================================================
//                          SYSTEM STATE QUERIES
// ==================================================================================

/**
 * Get System Uptime
 *
 * Returns the time elapsed since initialize() was called, in seconds.
 * Uses the ESP32's microsecond timer for precision.
 *
 * Returns:
 *   Seconds since system initialization (0 if not initialized)
 */
uint32_t SystemContext::getUptimeSeconds() const
{
    if (!is_initialized_)
    {
        return 0;
    }

    uint64_t now_us = esp_timer_get_time();
    uint64_t elapsed_us = now_us - init_time_us_;
    return static_cast<uint32_t>(elapsed_us / 1000000ULL);
}

/**
 * Get System Health Status
 *
 * Collects overall system health indicators and returns as bitmask.
 * This method queries each module for its health status.
 *
 * Health Indicators:
 *   Bit 0: Hardware driver ready
 *   Bit 1: Console queue healthy (not dropping messages)
 *   Bit 2: VU Meter task alive
 *   Bit 3: RDS Assembler task alive
 *   Bit 4: DSP Pipeline task alive
 *   Bit 5: CPU usage acceptable (< 50%)
 *   Bit 6: Heap usage acceptable (> 10% free)
 *
 * Returns:
 *   Bitmask of status flags
 *   0x00 = System perfectly healthy
 *   Non-zero = One or more issues detected
 *
 * Note:
 *   This is a stub implementation - full health checks would require
 *   each module to implement getHealthStatus() methods.
 */
uint32_t SystemContext::getHealthStatus() const
{
    uint32_t status = 0;

    if (!is_initialized_)
    {
        return 0xFF; // Not initialized = unhealthy
    }

    // Check hardware driver
    if (hardware_driver_ != nullptr && !hardware_driver_->isReady())
    {
        status |= 0x01; // Hardware not ready
    }

    // Additional health checks would be implemented here as modules
    // are enhanced with health monitoring APIs.

    return status;
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
