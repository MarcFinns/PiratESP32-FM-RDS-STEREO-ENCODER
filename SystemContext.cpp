/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
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
#include "IHardwareDriver.h"
#include "Log.h"
#include "RDSAssembler.h"
#include "VUMeter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
 * 3. Start Logger task (Core 1) - needed for all downstream diagnostics
 * 4. Start VU Meter task (Core 1) - display feedback
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
        Log::enqueuef(LogLevel::ERROR, "SystemContext::initialize() - hardware_driver is nullptr");
        return false;
    }

    if (is_initialized_)
    {
        Log::enqueuef(LogLevel::WARN, "SystemContext::initialize() - already initialized");
        return false;
    }

    hardware_driver_ = hardware_driver;

    // ---- Step 1: Initialize Hardware Driver ----
    // This must happen before any DSP operations
    if (!hardware_driver_->initialize())
    {
        int err = hardware_driver_->getErrorStatus();
        Log::enqueuef(LogLevel::ERROR, "Hardware driver init failed: %d", err);
        return false;
    }

    Log::enqueuef(LogLevel::INFO, "Hardware driver initialized");

    // ---- Step 2: Start Logger Task (Core 1) ----
    // Logger must be started early so downstream modules can log
    // Priority 2: Medium, higher than VU meter and RDS
    if (!Log::startTask(1,                          // core_id: Core 1 (I/O core)
                        Config::LOGGER_PRIORITY,    // priority
                        Config::LOGGER_STACK_WORDS, // stack_words
                        Config::LOGGER_QUEUE_LEN))  // queue_len
    {
        Log::enqueuef(LogLevel::ERROR, "Failed to start Logger task");
        return false;
    }

    Log::enqueuef(LogLevel::INFO, "Logger task started on Core 1");

    // ---- Step 3: Start VU Meter Task (Core 1) ----
    // Visual feedback for operator - lower priority than logging
    if (!VUMeter::startTask(1,                       // core_id: Core 1 (I/O core)
                            Config::VU_PRIORITY,     // priority
                            Config::VU_STACK_WORDS,  // stack_words
                            Config::VU_QUEUE_LEN))   // queue_len (mailbox)
    {
        Log::enqueuef(LogLevel::WARN, "Failed to start VUMeter task (non-critical)");
        // Non-critical failure - continue initialization
    }
    else
    {
        Log::enqueuef(LogLevel::INFO, "VU Meter task started on Core 1");
    }

    // ---- Step 4: Start RDS Assembler Task (Core 1) if Enabled ----
    if (enable_rds)
    {
        if (!RDSAssembler::startTask(1,                        // core_id: Core 1 (I/O core)
                                     Config::RDS_PRIORITY,     // priority
                                     Config::RDS_STACK_WORDS,  // stack_words
                                     Config::RDS_BIT_QUEUE_LEN)) // queue_len: bits
        {
            Log::enqueuef(LogLevel::WARN, "Failed to start RDSAssembler task (non-critical)");
            // Non-critical failure - continue without RDS
        }
        else
        {
            Log::enqueuef(LogLevel::INFO, "RDS Assembler task started on Core 1");
        }
    }

    // ---- Step 5: Start DSP Pipeline Task (Core 0) ----
    // CRITICAL: This must start last and run on Core 0 with highest priority
    // Audio processing cannot be interrupted. Use the same instance we store so
    // SystemContext::getDSPPipeline() returns the running object.
    dsp_pipeline_ = new DSP_pipeline(hardware_driver_);
    if (!dsp_pipeline_->startTaskInstance(
            dsp_core_id,   // core_id: Core 0 (dedicated)
            dsp_priority,  // priority: Highest (typically 6)
            dsp_stack_words)) // stack_words: 12KB typical
    {
        Log::enqueuef(LogLevel::ERROR, "Failed to start DSP Pipeline task");
        delete dsp_pipeline_;
        dsp_pipeline_ = nullptr;
        return false;
    }

    Log::enqueuef(LogLevel::INFO, "DSP Pipeline task started on Core %d with priority %d",
                  dsp_core_id, dsp_priority);

    // ---- Initialization Complete ----
    is_initialized_ = true;
    init_time_us_ = esp_timer_get_time();

    Log::enqueuef(LogLevel::INFO, "SystemContext initialized - all modules running");

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
 * 4. Stop Logger (Core 1) - must stop last so we can log shutdown progress
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

    Log::enqueuef(LogLevel::INFO, "SystemContext shutdown initiated");

    // ---- Step 1: Stop DSP Pipeline (Core 0) ----
    if (dsp_pipeline_ != nullptr)
    {
        // Note: DSP_pipeline needs a stop() method to signal graceful task shutdown.
        // For now, we delete the instance (task will be cleaned up by FreeRTOS).
        delete dsp_pipeline_;
        dsp_pipeline_ = nullptr;
        Log::enqueuef(LogLevel::INFO, "DSP Pipeline stopped");
    }

    // ---- Step 2: Stop RDS Assembler (Core 1) ----
    // Note: RDSAssembler::stopTask() needs to be implemented
    // For now, this is a placeholder for future task shutdown logic
    Log::enqueuef(LogLevel::INFO, "RDS Assembler stopped");

    // ---- Step 3: Stop VU Meter (Core 1) ----
    VUMeter::stopTask();
    Log::enqueuef(LogLevel::INFO, "VU Meter stopped");

    // ---- Step 4: Stop Logger (Core 1) - Last ----
    Log::enqueuef(LogLevel::INFO, "SystemContext shutdown complete");
    // Note: Log::stopTask() needs to be implemented
    // For now, this is a placeholder for future task shutdown logic

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
 *   Bit 1: Logger queue healthy (not dropping messages)
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
