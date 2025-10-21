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
 *     • Console task (priority 2)
 *     • VU Meter task (priority 1)
 *     • Handles serial output and display rendering
 *     • Cannot interrupt audio processing
 *
 * Task Priorities:
 *   Priority 6: DSP_pipeline  (highest - real-time audio)
 *   Priority 2: Console      (medium - CLI + diagnostics)
 *   Priority 1: VU Meter     (low - visual feedback)
 *   Priority 0: Idle         (system idle task)
 *
 * Memory Allocation:
 *   DSP_pipeline: 12,288 bytes stack (complex DSP operations)
 *   Console:      4,096 bytes stack (string formatting)
 *   VU Meter:     4,096 bytes stack (graphics rendering)
 *
 * Signal Flow:
 *   1. Audio enters via I2S RX (48 kHz stereo)
 *   2. DSP_pipeline processes 64-sample blocks (1.33 ms latency)
 *   3. DSP pipeline: pre-emphasis → notch → upsample → MPX synthesis
 *   4. Output via I2S TX (192 kHz stereo)
 *   5. VU meters display real-time levels on ILI9341 TFT
 *   6. Console handles CLI and outputs diagnostics to Serial (115200 baud)
 *
 * =====================================================================================
 */

#include "Config.h"
#include "Console.h"
#include "DSP_pipeline.h"
#include "DisplayManager.h"
#include "ESP32I2SDriver.h"
#include "RDSAssembler.h"
#include "SystemContext.h"
#include "splashscreen.h"
#include <Arduino_GFX_Library.h>
#include <pgmspace.h>

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

    // ---- Optional Splash Screen (before starting tasks) ----
    // Show a 320x240 RGB565 splash image for ~5 seconds, then continue normal init.
    // Safe to run here because the DisplayManager has not initialized yet.
    if (Config::VU_DISPLAY_ENABLED)
    {
        if (Config::TFT_BL >= 0)
        {
            pinMode((int)Config::TFT_BL, OUTPUT);
            digitalWrite((int)Config::TFT_BL, HIGH);
        }
        Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(
            Config::TFT_DC, Config::TFT_CS, Config::TFT_SCK, Config::TFT_MOSI, GFX_NOT_DEFINED, 1);
        Arduino_ILI9341 *gfx =
            new Arduino_ILI9341(bus, Config::TFT_RST, Config::TFT_ROTATION, false);
        if (gfx && gfx->begin())
        {
            gfx->fillScreen(0x0000);
            // Push the PROGMEM 320xH RGB565 image line by line
            const int splashW = 320;
            const int splashH = Config::SPLASH_HEIGHT;
            const int splashY0 = Config::SPLASH_TOP_Y;
            uint16_t line[320];
            for (int y = 0; y < splashH; ++y)
            {
                int drawY = splashY0 + y;
                if (drawY < 0 || drawY >= 240)
                    continue;
                for (int x = 0; x < splashW; ++x)
                    line[x] = pgm_read_word(&LOGO320[y * splashW + x]);
                gfx->draw16bitRGBBitmap(0, drawY, line, splashW, 1);
            }

            // ---- Overlay Text ----
            gfx->setTextWrap(false);
            gfx->setTextColor(0xFFFF); // white

            auto centerX = [](const char *s, int size) -> int
            {
                int w = (int)strlen(s) * 6 * size; // 6px per char at size 1
                int x = (320 - w) / 2;
                return x < 0 ? 0 : x;
            };

            // Title
            const char *title = "PiratESP";
            int title_size = 3;
            int title_y = 10;
            gfx->setTextSize(title_size);
            gfx->setCursor(centerX(title, title_size), title_y);
            gfx->print(title);

            // Subtitle one line under
            const char *subtitle = "FM MPX STEREO RDS ENCODER";
            int sub_size = 2;
            // Place roughly one line below title: 8 px base * title_size + small gap
            int sub_y = title_y + (8 * title_size) + 10;
            gfx->setTextSize(sub_size);
            gfx->setCursor(centerX(subtitle, sub_size), sub_y);
            gfx->print(subtitle);

            // Footer: build date/time and copyright at around y=200
            char build_line[96];
            snprintf(build_line, sizeof(build_line), "Build: %s %s  v%s", __DATE__, __TIME__,
                     Config::FIRMWARE_VERSION);
            const char *copy_line = "(c) MFINI 2025";
            int foot_size = 1;
            int foot_y = 215;
            gfx->setTextSize(foot_size);
            gfx->setCursor(centerX(build_line, foot_size), foot_y);
            gfx->print(build_line);
            gfx->setCursor(centerX(copy_line, foot_size), foot_y + 12);
            gfx->print(copy_line);
            delay(7000);
        }
        // Clean up temporary objects (DisplayManager will re-init later)
        delete gfx;
        delete bus;
    }

    // ---- Initialize System Context with Dependency Injection ----
    // Create and inject the hardware I/O driver
    static ESP32I2SDriver hw_driver;

    // Get the singleton SystemContext instance
    SystemContext &system = SystemContext::getInstance();

    // Initialize all system modules through the IoC container
    // This will start:
    //   1. Hardware driver (I2S initialization)
    //   2. Console task (Core 1, priority 2)
    //   3. VU Meter task (Core 1, priority 1)
    //   4. RDS Assembler task (Core 1, priority 1) if enabled
    //   5. DSP Pipeline task (Core 0, priority 6)
    bool initialized =
        system.initialize(&hw_driver,              // Injected hardware I/O driver
                          Config::DSP_CORE,        // dsp_core_id (from Config.h)
                          Config::DSP_PRIORITY,    // dsp_priority (from Config.h)
                          Config::DSP_STACK_WORDS, // dsp_stack_words (from Config.h)
                          Config::ENABLE_RDS_57K   // enable_rds: Enable RDS if configured
        );

    if (!initialized)
    {
        // Initialization failed - log error and halt
        Console::printOrSerial(LogLevel::ERROR, "FATAL: System initialization failed!");
        // In production, might implement watchdog recovery or graceful degradation
        while (true)
        {
            Serial.println("FATAL: System initialization failed!");
            delay(1000);
        }
    }

    // RDS parameters can now be set via the serial console (Console task)

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
