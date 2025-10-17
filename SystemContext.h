/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      System Context (IoC Container)
 *
 * =====================================================================================
 *
 * File:         SystemContext.h
 * Description:  Central dependency injection container for system modules
 *
 * Purpose:
 *   Manages the lifecycle and dependencies of all major system components:
 *   - DSP_pipeline (audio processing core)
 *   - Log (lock-free logging)
 *   - VUMeter (real-time level display)
 *   - RDSAssembler (RDS bitstream generation)
 *   - Hardware drivers (I2S abstraction)
 *
 * Design Pattern: Service Locator / IoC Container
 *   - Single instance (singleton) manages all module instances
 *   - Provides centralized initialization and shutdown
 *   - Enables easy module swapping (e.g., mock drivers for testing)
 *   - Clear dependency relationships
 *
 * Usage Example:
 *   SystemContext& system = SystemContext::getInstance();
 *   system.initialize(...);  // Start all modules
 *   // Modules are now running...
 *   system.shutdown();       // Clean shutdown
 *
 * =====================================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>

// Forward declarations
class IHardwareDriver;
class DSP_pipeline;

/**
 * SystemContext - Central IoC Container
 *
 * Manages lifecycle of all major system components. Provides a single point
 * to initialize, monitor, and shut down the entire audio processing system.
 *
 * Features:
 *   • Centralized module lifecycle management
 *   • Clear initialization order enforcement
 *   • Easy dependency injection for testing
 *   • System-wide state visibility
 *
 * Thread Safety:
 *   getInstance() is thread-safe (uses Meyer's static initialization).
 *   initialize() and shutdown() should only be called from main thread.
 *   Query methods (getUptimeSeconds, getHealthStatus, etc.) are thread-safe.
 *   Multiple tasks can call query methods concurrently without blocking.
 *
 * Initialization Order Enforcement:
 *   initialize() enforces a strict startup sequence to prevent race conditions:
 *   1. Hardware driver (injected dependency)
 *   2. Logger task (all downstream modules use logging)
 *   3. VU Meter task
 *   4. RDS Assembler task (if enabled)
 *   5. DSP Pipeline task (highest priority, last to start)
 *
 *   Shutdown follows reverse order to prevent use-after-free.
 */
class SystemContext
{
  public:
    /**
     * Get System Context Singleton Instance
     *
     * Thread-safe singleton accessor. Returns the single global instance
     * of SystemContext.
     *
     * Returns:
     *   Reference to the SystemContext singleton
     *
     * Example:
     *   SystemContext& system = SystemContext::getInstance();
     */
    static SystemContext &getInstance();

    // ==================================================================================
    //                          INITIALIZATION
    // ==================================================================================

    /**
     * Initialize All System Modules
     *
     * Starts all major system components in the correct order:
     * 1. Hardware driver initialization
     * 2. Logger task startup (Core 1)
     * 3. VU Meter task startup (Core 1)
     * 4. RDS Assembler task startup (Core 1)
     * 5. DSP Pipeline task startup (Core 0)
     *
     * Parameters:
     *   hardware_driver:  Injected hardware I/O driver
     *   dsp_core_id:      Core for DSP (typically 0)
     *   dsp_priority:     DSP task priority (typically 6)
     *   dsp_stack_words:  DSP stack size (typically 12288)
     *   enable_rds:       Whether to start RDS assembler
     *
     * Returns:
     *   true  - All modules initialized successfully
     *   false - Initialization failed (check logs)
     *
     * Note: Call exactly once during system startup in setup()
     */
    bool initialize(IHardwareDriver *hardware_driver, int dsp_core_id = 0,
                    UBaseType_t dsp_priority = 6, uint32_t dsp_stack_words = 12288,
                    bool enable_rds = true);

    /**
     * Shutdown All System Modules
     *
     * Cleanly shuts down all system components in reverse order.
     * Called during system shutdown or emergency stop.
     *
     * Note: After shutdown(), initialize() can be called again if needed
     */
    void shutdown();

    // ==================================================================================
    //                          MODULE ACCESSORS
    // ==================================================================================

    /**
     * Get DSP Pipeline Instance
     *
     * Returns pointer to the DSP_pipeline audio processing core.
     * Valid only after initialize() has been called.
     *
     * Returns:
     *   Pointer to DSP_pipeline instance, or nullptr if not initialized
     */
    DSP_pipeline *getDSPPipeline()
    {
        return dsp_pipeline_;
    }

    /**
     * Get Hardware Driver Instance
     *
     * Returns pointer to the hardware I/O driver.
     * Valid only after initialize() has been called.
     *
     * Returns:
     *   Pointer to IHardwareDriver instance, or nullptr if not initialized
     */
    IHardwareDriver *getHardwareDriver()
    {
        return hardware_driver_;
    }

    // ==================================================================================
    //                          SYSTEM STATE
    // ==================================================================================

    /**
     * Check if System is Initialized
     *
     * Returns:
     *   true  - System is initialized and running
     *   false - System not initialized or shutdown
     */
    bool isInitialized() const
    {
        return is_initialized_;
    }

    /**
     * Get System Uptime
     *
     * Returns:
     *   Seconds since system initialization (0 if not initialized)
     */
    uint32_t getUptimeSeconds() const;

    /**
     * Get System Health Status
     *
     * Collects overall system health indicators:
     *   • CPU usage percentage
     *   • Heap usage
     *   • Error counters
     *   • Task states
     *
     * Returns:
     *   Bitmask of status flags (0 = healthy, non-zero = issues)
     */
    uint32_t getHealthStatus() const;

  private:
    // ==================================================================================
    //                          PRIVATE STATE
    // ==================================================================================

    // Module instances (owned by SystemContext)
    IHardwareDriver *hardware_driver_;
    DSP_pipeline *dsp_pipeline_;

    // Initialization state
    bool is_initialized_;
    uint64_t init_time_us_;

    // ==================================================================================
    //                          PRIVATE METHODS
    // ==================================================================================

    /**
     * Private Constructor (Singleton Pattern)
     *
     * Prevents direct instantiation. Use getInstance() instead.
     */
    SystemContext();

    /**
     * Private Destructor
     *
     * Prevents accidental deletion of singleton.
     */
    ~SystemContext();

    // Prevent copying
    SystemContext(const SystemContext &) = delete;
    SystemContext &operator=(const SystemContext &) = delete;
};

// =====================================================================================
//                                END OF FILE
// =====================================================================================
