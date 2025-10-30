/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                        I2S Audio Driver Implementation
 *
 * =====================================================================================
 *
 * File:         I2SDriver.cpp
 * Description:  ESP32 I2S hardware initialization and configuration
 *
 * Purpose:
 *   This module implements low-level I2S (Inter-IC Sound) driver initialization
 *   for the ESP32's two I2S controllers. It configures dual independent audio
 *   interfaces with precise clock relationships for synchronous ADC/DAC operation.
 *
 * Hardware Details:
 *   The ESP32 I2S peripheral provides:
 *     • Two independent I2S controllers (I2S0 and I2S1)
 *     • Hardware DMA for zero-CPU audio transfers
 *     • Programmable MCLK generation (PLL-derived)
 *     • Support for various audio formats (standard I2S, left/right justified, etc.)
 *     • Configurable bit depths (8/16/24/32 bits)
 *     • Master or slave mode operation
 *
 * Clock Generation:
 *   The ESP32 generates I2S clocks from an internal PLL:
 *     1. Base clock: 160 MHz (from PLL, configurable)
 *     2. MCLK: Base clock ÷ divider = 24.576 MHz
 *     3. BCK: MCLK ÷ 2 (for TX: 12.288 MHz, for RX: 3.072 MHz)
 *     4. LRCK: BCK ÷ 64 (TX: Config::SAMPLE_RATE_DAC, RX: Config::SAMPLE_RATE_ADC)
 *
 * I2S Format:
 *   Standard I2S (Philips format):
 *     • MSB-first transmission
 *     • Data delayed by 1 BCK from LRCK edge
 *     • LRCK low = left channel, LRCK high = right channel
 *     • 32-bit word size (24-bit audio MSB-aligned in 32-bit word)
 *
 * DMA Buffer Management:
 *   TX uses 6 buffers × 256 samples; RX uses 6 buffers × 64 samples:
 *     • TX total: 1536 samples; latency ≈ 1536 ÷ SAMPLE_RATE_DAC
 *     • RX total:  384 samples; latency ≈  384 ÷ SAMPLE_RATE_ADC
 *   DMA operates in circular buffer mode; CPU only services read/write calls.
 *
 * Initialization Order:
 *   1. setupTx() must be called first (generates MCLK)
 *   2. setupRx() can then be called (shares MCLK from TX)
 *   3. Both interfaces run independently with synchronized clocks
 *
 * =====================================================================================
 */

#include "I2SDriver.h"

#include "Config.h"
#include "Console.h"

#include <Arduino.h>
#include <cstdarg>
#include <driver/i2s.h>

namespace AudioIO
{
    // ==================================================================================
    //                          I2S PORT ASSIGNMENTS
    // ==================================================================================

    // ---- Anonymous Namespace for Internal Constants ----
    //
    // These constants define which ESP32 I2S hardware controller is used for
    // TX (output) and RX (input). They are not exposed in the header to keep
    // the interface hardware-agnostic.

    namespace
    {
        inline void log_via_logger_or_serial(LogLevel level, const char *fmt, ...)
        {
            char buf[160];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            if (!Console::enqueue(level, buf))
            {
                if (level == LogLevel::ERROR)
                {
                    Serial.printf("[ERROR] %s\n", buf);
                }
                else
                {
                    Serial.println(buf);
                }
            }
        }
        /**
         * I2S Port for TX (DAC Output)
         *
         * Uses I2S Port 0 (I2S_NUM_0) for DAC stereo output.
         * This port generates the 24.576 MHz MCLK shared with RX.
         */
        const i2s_port_t kI2SPortTx = I2S_NUM_0;

        /**
         * I2S Port for RX (ADC Input)
         *
         * Uses I2S Port 1 (I2S_NUM_1) for ADC stereo input.
         * This port shares MCLK from TX port for synchronization.
         */
        const i2s_port_t kI2SPortRx = I2S_NUM_1;

    } // anonymous namespace

    // ==================================================================================
    //                          I2S TX INITIALIZATION
    // ==================================================================================

    /**
     * Initialize I2S TX Interface (DAC Output @ Config::SAMPLE_RATE_DAC)
     *
     * Configures I2S Port 0 for high-speed stereo output. This function performs:
     *   1. I2S driver installation with DMA buffer configuration
     *   2. GPIO pin assignment for MCLK, BCK, LRCK, and DOUT
     *   3. Diagnostic output to Serial with clock frequencies
     *
     * Returns:
     *   true if both driver installation and pin configuration succeed
     *   false if any step fails (with error message to Serial)
     */
    bool setupTx()
    {
        // Import Config constants into local scope for cleaner syntax
        using namespace Config;

        // ---- Log Initialization Start ----
        log_via_logger_or_serial(LogLevel::INFO, "Initializing I2S TX (DAC @ %u Hz)...",
                                  (unsigned)SAMPLE_RATE_DAC);

        // ---- Configure I2S Driver (External I2S DAC) ----
        i2s_config_t config{};
        {
            config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
            config.sample_rate = SAMPLE_RATE_DAC;
            config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
            config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
            config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
            config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
            config.dma_buf_count = 6;
            config.dma_buf_len = Config::I2S_DMA_LEN_TX;
            config.use_apll = true;
            config.tx_desc_auto_clear = true;
            config.fixed_mclk = 0;
            config.mclk_multiple = I2S_MCLK_MULTIPLE_128;
            config.bits_per_chan = I2S_BITS_PER_CHAN_32BIT;
        }

        // ---- Install I2S Driver ----
        //
        // i2s_driver_install() configures the hardware and allocates DMA buffers.
        // Queue size = 0 means no event queue (we use blocking I/O).
        esp_err_t ret = i2s_driver_install(kI2SPortTx, &config, 0, nullptr);
        if (ret != ESP_OK)
        {
            // Driver installation failed (likely out of memory or invalid params)
            log_via_logger_or_serial(LogLevel::ERROR, "Failed to install TX driver: %d", ret);
            return false;
        }

        {
            i2s_pin_config_t pins = {
                .mck_io_num = PIN_MCLK,
                .bck_io_num = PIN_DAC_BCK,
                .ws_io_num = PIN_DAC_LRCK,
                .data_out_num = PIN_DAC_DOUT,
                .data_in_num = I2S_PIN_NO_CHANGE
            };
            ret = i2s_set_pin(kI2SPortTx, &pins);
        }
        if (ret != ESP_OK)
        {
            // Pin configuration failed (likely invalid GPIO numbers)
            log_via_logger_or_serial(LogLevel::ERROR, "Failed to set TX pins: %d", ret);
            return false;
        }

        // Ensure TX engine is running with a clean DMA buffer
        i2s_zero_dma_buffer(kI2SPortTx);
        i2s_start(kI2SPortTx);

        // ---- Log Success and Clock Frequencies ----
        //
        // Print diagnostic information to help verify correct configuration.
        log_via_logger_or_serial(LogLevel::INFO, "I2S TX initialized successfully");
        log_via_logger_or_serial(LogLevel::INFO, "  Sample Rate: %u Hz", SAMPLE_RATE_DAC);

        // MCLK frequency: SAMPLE_RATE_DAC × 128
        log_via_logger_or_serial(LogLevel::INFO, "  MCLK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_DAC * 128) / 1'000'000.0f, PIN_MCLK);

        log_via_logger_or_serial(LogLevel::INFO, "  BCK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_DAC * 64) / 1'000'000.0f, PIN_DAC_BCK);
        log_via_logger_or_serial(LogLevel::INFO, "  LRCK: %u Hz (GPIO%d)", SAMPLE_RATE_DAC, PIN_DAC_LRCK);

        return true;
    }

    // ==================================================================================
    //                          I2S RX INITIALIZATION
    // ==================================================================================

    /**
     * Initialize I2S RX Interface (ADC Input @ Config::SAMPLE_RATE_ADC)
     *
     * Configures I2S Port 1 for stereo input. This function performs:
     *   1. I2S driver installation with DMA buffer configuration
     *   2. GPIO pin assignment for BCK, LRCK, and DIN
     *   3. Diagnostic output to Serial with clock frequencies
     *
     * Important: setupTx() must be called first to generate MCLK.
     *
     * Returns:
     *   true if both driver installation and pin configuration succeed
     *   false if any step fails (with error message to Serial)
     */
    bool setupRx()
    {
        // Import Config constants into local scope
        using namespace Config;

        // ---- Log Initialization Start ----
        log_via_logger_or_serial(LogLevel::INFO, "Initializing I2S RX (ADC @ %u Hz)...",
                                  (unsigned)SAMPLE_RATE_ADC);

        // ---- Configure I2S Driver ----
        //
        // Configuration is similar to TX but with key differences:
        //   • Mode: I2S_MODE_RX (receive instead of transmit)
            //   • Sample rate: SAMPLE_RATE_ADC (typically 1/4 of TX)
        //   • MCLK multiple: 512 (instead of 128) to match shared 24.576 MHz MCLK

        i2s_config_t config = {
            // ---- Operating Mode ----
            // I2S_MODE_MASTER: ESP32 generates clocks (ADC is slave)
            // I2S_MODE_RX: Receive mode (data flows ADC → ESP32)
            .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),

            // ---- Sample Rate ----
            // Input LRCK, from Config::SAMPLE_RATE_ADC (e.g., 44,100 Hz)
            .sample_rate = SAMPLE_RATE_ADC,

            // ---- Bit Depth ----
            // I2S_BITS_PER_SAMPLE_32BIT: 32-bit words on the wire
            // Actual audio is 24-bit MSB-aligned within 32-bit word
            .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

            // ---- Channel Format ----
            // I2S_CHANNEL_FMT_RIGHT_LEFT: Stereo (L + R interleaved)
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,

            // ---- Communication Format ----
            // I2S_COMM_FORMAT_STAND_I2S: Standard I2S format
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,

            // ---- Interrupt Priority ----
            // ESP_INTR_FLAG_LEVEL1: Low priority (DMA-driven)
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

            // ---- DMA Buffer Configuration ----
            // Align RX buffer length with input DSP block (64 frames).
            .dma_buf_count = 6,
            .dma_buf_len = Config::I2S_DMA_LEN_RX,

            // ---- Clock Source ----
            // Use APLL for precise 44.1k-family clocking
            .use_apll = true,

            // ---- TX Descriptor Auto-Clear ----
            // tx_desc_auto_clear: true (unused for RX, but set for consistency)
            .tx_desc_auto_clear = true,

            // ---- MCLK Configuration ----
            // fixed_mclk: 0 = calculate from mclk_multiple
            // mclk_multiple: 512 → MCLK = 512 × sample_rate
            //   Result: 44,100 × 512 = 22.5792 MHz (matches TX)
            .fixed_mclk = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_512,

            // ---- Channel Bit Width ----
            // I2S_BITS_PER_CHAN_32BIT: 32 bits per channel
            .bits_per_chan = I2S_BITS_PER_CHAN_32BIT
        };

        // ---- Install I2S Driver ----
        //
        // Allocates DMA buffers and configures I2S Port 1 hardware.
        esp_err_t ret = i2s_driver_install(kI2SPortRx, &config, 0, nullptr);
        if (ret != ESP_OK)
        {
            // Driver installation failed
            log_via_logger_or_serial(LogLevel::ERROR, "Failed to install RX driver: %d", ret);
            return false;
        }

        // ---- Configure GPIO Pins ----
        //
        // Pin assignment for RX interface. Note that MCLK is not configured
        // here because it's already generated by the TX interface.

        // Choose RX pins; on classic ESP32 with internal DAC, avoid GPIO25/26 for RX clocks
        int rx_bck = PIN_ADC_BCK;
        int rx_ws  = PIN_ADC_LRCK;

        i2s_pin_config_t pins = {
            .mck_io_num = I2S_PIN_NO_CHANGE,
            .bck_io_num = rx_bck,
            .ws_io_num = rx_ws,
            .data_out_num = I2S_PIN_NO_CHANGE,
            .data_in_num = PIN_ADC_DIN
        };

        // ---- Apply Pin Configuration ----
        ret = i2s_set_pin(kI2SPortRx, &pins);
        if (ret != ESP_OK)
        {
            // Pin configuration failed
            log_via_logger_or_serial(LogLevel::ERROR, "Failed to set RX pins: %d", ret);
            return false;
        }

        // ---- Log Success and Clock Frequencies ----
        log_via_logger_or_serial(LogLevel::INFO, "I2S RX initialized successfully");
        log_via_logger_or_serial(LogLevel::INFO, "  Sample Rate: %u Hz", SAMPLE_RATE_ADC);

        // MCLK frequency: SAMPLE_RATE_ADC × 512
        log_via_logger_or_serial(LogLevel::INFO, "  MCLK: %.3f MHz (from TX GPIO%d)",
                                 (SAMPLE_RATE_ADC * 512) / 1'000'000.0f, PIN_MCLK);

        // BCK frequency: SAMPLE_RATE_ADC × 64
        log_via_logger_or_serial(LogLevel::INFO, "  BCK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_ADC * 64) / 1'000'000.0f, PIN_ADC_BCK);

        // LRCK frequency: Same as sample rate (SAMPLE_RATE_ADC)
        log_via_logger_or_serial(LogLevel::INFO, "  LRCK: %u Hz (GPIO%d)", SAMPLE_RATE_ADC, PIN_ADC_LRCK);

        return true;
    }

    // ==================================================================================
    //                          I2S SHUTDOWN
    // ==================================================================================

    /**
     * Shutdown Both I2S Interfaces
     *
     * Uninstalls I2S drivers for both TX and RX, releasing DMA buffers and
     * freeing GPIO pins. Hardware peripherals are disabled.
     *
     * Execution Flow:
     *   1. Uninstall TX driver (I2S Port 0)
     *   2. Uninstall RX driver (I2S Port 1)
     *
     * Note: This function does not return status. It always succeeds, even if
     *       the drivers were not previously installed.
     */
    void shutdown()
    {
        // Uninstall TX driver (releases DMA buffers and GPIO pins)
        i2s_driver_uninstall(kI2SPortTx);

        // Uninstall RX driver (releases DMA buffers and GPIO pins)
        i2s_driver_uninstall(kI2SPortRx);
    }
    
    int getTxPort() { return (int)kI2SPortTx; }
    int getRxPort() { return (int)kI2SPortRx; }

    void emitHardwareRecap()
    {
        using namespace Config;
        // Emit a neat, multi-line recap with short lines (<= 120 chars)
        // to avoid truncation by the logger line limit.
        auto line = [](const char *fmt, auto... args) {
            char buf[128];
            snprintf(buf, sizeof(buf), fmt, args...);
            (void)Console::enqueue(LogLevel::INFO, buf);
        };

        Console::enqueue(LogLevel::INFO, "");
        Console::enqueue(LogLevel::INFO, "==================== HARDWARE RECAP ====================");
#if defined(PROJ_TARGET_ESP32S3)
        Console::enqueue(LogLevel::INFO, "Target: ESP32-S3");
#elif defined(PROJ_TARGET_ESP32)
        Console::enqueue(LogLevel::INFO, "Target: ESP32 (classic)");
#else
        Console::enqueue(LogLevel::INFO, "Target: Unknown (defaulted config)");
#endif

        Console::enqueue(LogLevel::INFO, "-- I2S TX (DAC)");
        line("  Port: %d (external DAC, I2S slave)", getTxPort());
        line("  Rate: %u Hz, Bits: sample=32, chan=32", (unsigned)SAMPLE_RATE_DAC);
        line("  Format: ch=RIGHT_LEFT, comm=I2S, APLL=on, MCLKx=128");
        line("  Pins: MCLK=GPIO%d, BCK=GPIO%d, LRCK=GPIO%d, DOUT=GPIO%d",
             PIN_MCLK, PIN_DAC_BCK, PIN_DAC_LRCK, PIN_DAC_DOUT);
        line("  Clocks: MCLK=%.3f MHz, BCK=%.3f MHz, LRCK=%u Hz",
             (SAMPLE_RATE_DAC * 128) / 1'000'000.0f,
             (SAMPLE_RATE_DAC * 64) / 1'000'000.0f,
             (unsigned)SAMPLE_RATE_DAC);
        line("  DMA: count=%d, len=%d samples", 6, I2S_DMA_LEN_TX);

        Console::enqueue(LogLevel::INFO, "-- I2S RX (ADC)");
        line("  Port: %d (external ADC, I2S slave)", getRxPort());
        line("  Rate: %u Hz, Bits: sample=32, chan=32", (unsigned)SAMPLE_RATE_ADC);
        line("  Format: ch=RIGHT_LEFT, comm=I2S, APLL=on, MCLKx=512");

        {
            line("  Pins: MCLK=GPIO%d (from TX), BCK=GPIO%d, LRCK=GPIO%d, DIN=GPIO%d",
                 PIN_MCLK, PIN_ADC_BCK, PIN_ADC_LRCK, PIN_ADC_DIN);
        }
        line("  Clocks: MCLK=%.3f MHz, BCK=%.3f MHz, LRCK=%u Hz",
             (SAMPLE_RATE_ADC * 512) / 1'000'000.0f,
             (SAMPLE_RATE_ADC * 64) / 1'000'000.0f,
             (unsigned)SAMPLE_RATE_ADC);
        line("  DMA: count=%d, len=%d samples", 6, I2S_DMA_LEN_RX);

        Console::enqueue(LogLevel::INFO, "-- Display (ILI9341 SPI)");
        line("  Pins: SCK=GPIO%d, MOSI=GPIO%d, DC=GPIO%d, CS=GPIO%d, RST=GPIO%d, BL=%d",
             TFT_SCK, TFT_MOSI, TFT_DC, TFT_CS, TFT_RST, TFT_BL);

        Console::enqueue(LogLevel::INFO, "========================================================");
    }
    

} // namespace AudioIO

// =====================================================================================
//                                END OF FILE
// =====================================================================================
