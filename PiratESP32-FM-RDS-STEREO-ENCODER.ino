/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                              Main Application
 *
 * =====================================================================================
 *
 * File:         PiratESP32-FM-RDS-STEREO-ENCODER.ino
 * Description:  Entry point for ESP32 FM stereo encoder with real-time DSP
 *
 * Overview:
 *   This application implements a professional-grade FM stereo encoder using the
 *   ESP32's dual-core architecture. It receives stereo audio at 48 kHz via I2S,
 *   processes it through a multi-stage DSP pipeline, and outputs FM-multiplexed
 *   stereo at 192 kHz for transmission.
 *
 * Core Architecture:
 *   CORE 0 (Real-Time Audio):
 *     • DSP_pipeline task (priority 6 - highest)
 *     • Handles all audio I/O and DSP processing
 *     • Must maintain strict timing for glitch-free audio
 *
 *   CORE 1 (Non-Real-Time):
 *     • Logger task (priority 2)
 *     • VU Meter task (priority 1)
 *     • Handles serial output and display rendering
 *     • Cannot interrupt audio processing
 *
 * Task Priorities:
 *   Priority 6: DSP_pipeline  (highest - real-time audio)
 *   Priority 2: Logger       (medium - diagnostics)
 *   Priority 1: VU Meter     (low - visual feedback)
 *   Priority 0: Idle         (system idle task)
 *
 * Memory Allocation:
 *   DSP_pipeline: 12,288 bytes stack (complex DSP operations)
 *   Logger:       4,096 bytes stack (string formatting)
 *   VU Meter:     4,096 bytes stack (graphics rendering)
 *
 * Signal Flow:
 *   1. Audio enters via I2S RX (48 kHz stereo)
 *   2. DSP_pipeline processes 64-sample blocks (1.33 ms latency)
 *   3. DSP pipeline: pre-emphasis → notch → upsample → MPX synthesis
 *   4. Output via I2S TX (192 kHz stereo)
 *   5. VU meters display real-time levels on ILI9341 TFT
 *   6. Logger outputs diagnostics to Serial (115200 baud)
 *
 * =====================================================================================
 */

#include "DSP_pipeline.h"
#include "ESP32I2SDriver.h"
#include "Log.h"
#include "RDSAssembler.h"
#include "SystemContext.h"
#include "VUMeter.h"

// Optional: RDS helper/demo task (Core 1)
static void rds_demo_task(void *arg)
{
    (void)arg;
    // Initial service identification
    RDSAssembler::setPI(0x52A1);     // Example PI code
    RDSAssembler::setPTY(10);        // PTY = Pop Music (example)
    RDSAssembler::setTP(true);       // Traffic Program
    RDSAssembler::setTA(false);      // No current Traffic Announcement
    RDSAssembler::setMS(true);       // Music
    RDSAssembler::setPS("PiratESP"); // 8 chars, padded with space

    // A few rotating RadioText messages (64 chars max; padded internally)
    static const char *kRT[] = {"Hello from ESP32 FM Stereo RDS encoder!",
                                "Fully digital signal processing pipeline",
                                "This is a demo RadioText. Enjoy the music!"};

    const int n = sizeof(kRT) / sizeof(kRT[0]);
    int idx = 0;

    // Initial RT
    RDSAssembler::setRT(kRT[idx]);
    idx = (idx + 1) % n;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(30000)); // update every 30 seconds
        RDSAssembler::setRT(kRT[idx]);    // setRT toggles A/B flag internally
        idx = (idx + 1) % n;
    }
}

// ==================================================================================
//                              ARDUINO SETUP
// ==================================================================================

/**
 * Initialize hardware and start FreeRTOS tasks
 *
 * Uses SystemContext (IoC Container) to manage all module initialization.
 * This centralized approach ensures:
 *   • Consistent startup order
 *   • Proper dependency injection
 *   • Clear lifecycle management
 *   • Easy to test with mock drivers
 *
 * Core Assignment Strategy:
 *   • Core 0: Dedicated to audio processing (no interruptions)
 *   • Core 1: Handles all I/O (serial, display, logging)
 *   • This separation ensures audio processing is never blocked by I/O
 */
void setup()
{
    // ---- Initialize Serial Communication ----
    // 115200 baud for high-speed diagnostic output
    Serial.begin(115200);
    delay(100); // Allow Serial to stabilize

    // ---- Initialize System Context with Dependency Injection ----
    // Create and inject the hardware I/O driver
    static ESP32I2SDriver hw_driver;

    // Get the singleton SystemContext instance
    SystemContext &system = SystemContext::getInstance();

    // Initialize all system modules through the IoC container
    // This will start:
    //   1. Hardware driver (I2S initialization)
    //   2. Logger task (Core 1, priority 2)
    //   3. VU Meter task (Core 1, priority 1)
    //   4. RDS Assembler task (Core 1, priority 1) if enabled
    //   5. DSP Pipeline task (Core 0, priority 6)
    bool initialized =
        system.initialize(&hw_driver,            // Injected hardware I/O driver
                          0,                     // dsp_core_id: Core 0 (dedicated audio)
                          6,                     // dsp_priority: Highest priority
                          12288,                 // dsp_stack_words: 12KB for DSP buffers
                          Config::ENABLE_RDS_57K // enable_rds: Enable RDS if configured
        );

    if (!initialized)
    {
        // Initialization failed - log error and halt
        // In production, might implement watchdog recovery or graceful degradation
        while (true)
        {
            Serial.println("FATAL: System initialization failed!");
            delay(1000);
        }
    }

    // ---- Start RDS Demo Task (Optional) ----
    // Only spawn demo task if RDS is enabled
    if (Config::ENABLE_RDS_57K)
    {
        // Spawn the RDS demo helper on Core 1 with low priority
        xTaskCreatePinnedToCore(rds_demo_task, // Task function
                                "rds_demo",    // Task name
                                2048,          // Stack size in words
                                nullptr,       // Task parameter
                                1,             // Priority (low)
                                nullptr,       // Task handle (not needed)
                                1);            // Core ID (Core 1)
    }

    // At this point, all system modules are running and ready
    // Arduino loop() will yield to FreeRTOS scheduler
}

// ==================================================================================
//                              ARDUINO LOOP
// ==================================================================================

/**
 * Main loop (runs on Core 1)
 *
 * Since all real work is done by FreeRTOS tasks, this loop simply yields
 * to the scheduler. The vTaskDelay(1) call prevents busy-waiting and allows
 * other tasks to run efficiently.
 *
 * Note: This is NOT the idle task - it's a low-priority Arduino loop task.
 * The actual FreeRTOS idle task runs at priority 0 on both cores.
 */
void loop()
{
    // Yield to FreeRTOS scheduler
    // Delay of 1 tick (~1ms) prevents this loop from consuming CPU
    vTaskDelay(1);
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
