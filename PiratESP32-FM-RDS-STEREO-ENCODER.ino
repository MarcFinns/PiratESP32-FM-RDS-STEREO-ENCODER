/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                              Main Application
 *
 * =====================================================================================
 *
 * File:         ESP32_RDS_STEREO_SW_ENCODER_GIT.ino
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
#include "Log.h"
#include "RDSAssembler.h"
#include "VUMeter.h"

// Optional: RDS helper/demo task (Core 1)
static void rds_demo_task(void* arg)
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
    static const char* kRT[] = {"Hello from ESP32 FM Stereo RDS encoder!",
                                "Fully digital signal processing pipeline",
                                "This is a demo RadioText. Enjoy the music!"};

    const int n   = sizeof(kRT) / sizeof(kRT[0]);
    int       idx = 0;

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
 * Task Startup Order:
 *   1. Serial communication (for logging)
 *   2. Logger task (core 1, priority 2)
 *   3. VU Meter task (core 1, priority 1)
 *   4. DSP_pipeline task (core 0, priority 6)
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

    // ---- Start Logger Task (Core 1) ----
    // Handles all Serial.print() calls from audio thread via queue
    // Queue size: 128 messages (prevents audio blocking on logging)
    // Priority 2: Higher than VU meter, lower than audio
    Log::startTask(1,    // core_id: Core 1 (I/O core)
                   2,    // priority: Medium priority
                   4096, // stack_words: 4KB stack
                   128   // queue_len: 128 message buffer
    );

    // ---- Start VU Meter Task (Core 1) ----
    // Renders real-time audio levels on ILI9341 display
    // Queue size: 1 (mailbox pattern - only latest sample matters)
    // Priority 1: Lower than logger (visual feedback can wait)
    VUMeter::startTask(1,    // core_id: Core 1 (same as logger)
                       1,    // priority: Low priority
                       4096, // stack_words: 4KB stack
                       1     // queue_len: Single-slot mailbox
    );

    // ---- Start RDS Assembler Task (Core 1) ----
    // Non-real-time task to build RDS bitstream; audio core will synthesize
    if (Config::ENABLE_RDS_57K)
    {
        RDSAssembler::startTask(1,    // core_id: Core 1
                                1,    // priority: Low
                                4096, // stack_words
                                1024  // bit queue length
        );

        // Spawn the helper on Core 1 with low priority and modest stack
        xTaskCreatePinnedToCore(rds_demo_task, "rds_demo", 2048, nullptr, 1, nullptr, 1);
    }

    // ---- Start Audio Engine Task (Core 0) ----
    // Real-time audio processing - highest priority
    // Runs independently on Core 0 for deterministic timing
    // Priority 6: Highest priority (audio cannot be interrupted)
    DSP_pipeline::startTask(0,    // core_id: Core 0 (dedicated audio core)
                            6,    // priority: Highest priority
                            12288 // stack_words: 12KB stack (DSP buffers + FreeRTOS overhead)
    );

    // At this point, all three tasks are running independently
    // Arduino loop() becomes idle and yields to FreeRTOS scheduler
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
