/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
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
 *     4. LRCK: BCK ÷ 64 (for TX: 192 kHz, for RX: 48 kHz)
 *
 * I2S Format:
 *   Standard I2S (Philips format):
 *     • MSB-first transmission
 *     • Data delayed by 1 BCK from LRCK edge
 *     • LRCK low = left channel, LRCK high = right channel
 *     • 32-bit word size (24-bit audio MSB-aligned in 32-bit word)
 *
 * DMA Buffer Management:
 *   Each I2S interface uses 6 DMA buffers of 240 samples each:
 *     • Total buffer: 1440 samples = 2880 bytes per channel
 *     • TX latency: 1440 ÷ 192000 = 7.5 ms
 *     • RX latency: 1440 ÷ 48000 = 30 ms
 *   DMA operates in circular buffer mode, no CPU intervention required.
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
         * Uses I2S Port 1 (I2S_NUM_1) for 192 kHz stereo output.
         * This port generates the 24.576 MHz MCLK shared with RX.
         */
        const i2s_port_t kI2SPortTx = I2S_NUM_1;

        /**
         * I2S Port for RX (ADC Input)
         *
         * Uses I2S Port 0 (I2S_NUM_0) for 48 kHz stereo input.
         * This port shares MCLK from TX port for synchronization.
         */
        const i2s_port_t kI2SPortRx = I2S_NUM_0;

    } // anonymous namespace

    // ==================================================================================
    //                          I2S TX INITIALIZATION
    // ==================================================================================

    /**
     * Initialize I2S TX Interface (DAC Output @ 192 kHz)
     *
     * Configures I2S Port 1 for high-speed stereo output. This function performs:
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
        log_via_logger_or_serial(LogLevel::INFO, "Initializing I2S TX (DAC @ 192kHz)...");

        // ---- Configure I2S Driver ----
        //
        // i2s_config_t defines the I2S operational parameters.
        // This struct is passed to i2s_driver_install() to configure hardware.

        i2s_config_t config = {
            // ---- Operating Mode ----
            // I2S_MODE_MASTER: ESP32 generates all clocks (MCLK, BCK, LRCK)
            // I2S_MODE_TX: Transmit mode (data flows ESP32 → DAC)
            .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),

            // ---- Sample Rate ----
            // Sample rate in Hz (192,000 Hz from Config)
            // Determines LRCK frequency (word select toggles at this rate)
            .sample_rate = SAMPLE_RATE_DAC,

            // ---- Bit Depth ----
            // I2S_BITS_PER_SAMPLE_32BIT: 32-bit words on the wire
            // Actual audio is 24-bit MSB-aligned within 32-bit word
            .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,

            // ---- Channel Format ----
            // I2S_CHANNEL_FMT_RIGHT_LEFT: Stereo (left + right channels)
            // Data is interleaved: L, R, L, R, ...
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,

            // ---- Communication Format ----
            // I2S_COMM_FORMAT_STAND_I2S: Standard I2S (Philips format)
            // Data delayed by 1 BCK from LRCK edge
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,

            // ---- Interrupt Priority ----
            // ESP_INTR_FLAG_LEVEL1: Low interrupt priority
            // DMA handles transfers, interrupts only fire on buffer completion
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

            // ---- DMA Buffer Configuration ----
            // dma_buf_count: Number of DMA buffers (6 = triple buffering)
            // dma_buf_len: Samples per buffer (240 samples = 1.25 ms @ 192 kHz)
            // Total latency: 6 × 1.25 ms = 7.5 ms
            .dma_buf_count = 6,
            .dma_buf_len = 240,

            // ---- Clock Source ----
            // use_apll: false = use internal PLL (more stable, lower jitter)
            // true would use APLL (audio PLL) for lower clock noise
            .use_apll = false,

            // ---- TX Descriptor Auto-Clear ----
            // tx_desc_auto_clear: true = clear DMA descriptor on underrun
            // Prevents repeating old audio data if buffer underruns occur
            .tx_desc_auto_clear = true,

            // ---- MCLK Configuration ----
            // fixed_mclk: 0 = calculate MCLK from mclk_multiple
            // mclk_multiple: I2S_MCLK_MULTIPLE_128 = MCLK = 128 × sample_rate
            //   Result: 192,000 Hz × 128 = 24.576 MHz MCLK
            .fixed_mclk = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_128,

            // ---- Channel Bit Width ----
            // I2S_BITS_PER_CHAN_32BIT: 32 bits per channel on the wire
            .bits_per_chan = I2S_BITS_PER_CHAN_32BIT
        };

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

        // ---- Configure GPIO Pins ----
        //
        // i2s_pin_config_t maps I2S signals to physical GPIO pins.
        // Pin numbers come from Config namespace.

        i2s_pin_config_t pins = {
            // MCLK output: 24.576 MHz master clock (shared with ADC)
            .mck_io_num = PIN_MCLK,

            // BCK output: Bit clock (12.288 MHz = 192k × 64 bits per frame)
            .bck_io_num = PIN_DAC_BCK,

            // WS (LRCK) output: Word select / left-right clock (192 kHz)
            .ws_io_num = PIN_DAC_LRCK,

            // Data output: Serial audio data to DAC
            .data_out_num = PIN_DAC_DOUT,

            // Data input: Not used for TX (set to I2S_PIN_NO_CHANGE)
            .data_in_num = I2S_PIN_NO_CHANGE
        };

        // ---- Apply Pin Configuration ----
        ret = i2s_set_pin(kI2SPortTx, &pins);
        if (ret != ESP_OK)
        {
            // Pin configuration failed (likely invalid GPIO numbers)
            log_via_logger_or_serial(LogLevel::ERROR, "Failed to set TX pins: %d", ret);
            return false;
        }

        // ---- Log Success and Clock Frequencies ----
        //
        // Print diagnostic information to help verify correct configuration.
        log_via_logger_or_serial(LogLevel::INFO, "I2S TX initialized successfully");
        log_via_logger_or_serial(LogLevel::INFO, "  Sample Rate: %u Hz", SAMPLE_RATE_DAC);

        // MCLK frequency: 192,000 Hz × 128 = 24.576 MHz
        log_via_logger_or_serial(LogLevel::INFO, "  MCLK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_DAC * 128) / 1'000'000.0f, PIN_MCLK);

        // BCK frequency: 192,000 Hz × 64 = 12.288 MHz
        // (64 = 32 bits × 2 channels per sample period)
        log_via_logger_or_serial(LogLevel::INFO, "  BCK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_DAC * 64) / 1'000'000.0f, PIN_DAC_BCK);

        // LRCK frequency: Same as sample rate (192 kHz)
        log_via_logger_or_serial(LogLevel::INFO, "  LRCK: %u Hz (GPIO%d)", SAMPLE_RATE_DAC, PIN_DAC_LRCK);

        return true;
    }

    // ==================================================================================
    //                          I2S RX INITIALIZATION
    // ==================================================================================

    /**
     * Initialize I2S RX Interface (ADC Input @ 48 kHz)
     *
     * Configures I2S Port 0 for stereo input. This function performs:
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
        log_via_logger_or_serial(LogLevel::INFO, "Initializing I2S RX (ADC @ 48kHz)...");

        // ---- Configure I2S Driver ----
        //
        // Configuration is similar to TX but with key differences:
        //   • Mode: I2S_MODE_RX (receive instead of transmit)
        //   • Sample rate: 48 kHz (4× slower than TX)
        //   • MCLK multiple: 512 (instead of 128) to match shared 24.576 MHz MCLK

        i2s_config_t config = {
            // ---- Operating Mode ----
            // I2S_MODE_MASTER: ESP32 generates clocks (ADC is slave)
            // I2S_MODE_RX: Receive mode (data flows ADC → ESP32)
            .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),

            // ---- Sample Rate ----
            // Sample rate in Hz (48,000 Hz from Config)
            // LRCK toggles at this rate to indicate L/R channels
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
            // dma_buf_count: 6 buffers for triple buffering
            // dma_buf_len: 240 samples per buffer (5 ms @ 48 kHz)
            // Total latency: 6 × 5 ms = 30 ms
            .dma_buf_count = 6,
            .dma_buf_len = 240,

            // ---- Clock Source ----
            // use_apll: false = use internal PLL (same as TX for sync)
            .use_apll = false,

            // ---- TX Descriptor Auto-Clear ----
            // tx_desc_auto_clear: true (unused for RX, but set for consistency)
            .tx_desc_auto_clear = true,

            // ---- MCLK Configuration ----
            // fixed_mclk: 0 = calculate from mclk_multiple
            // mclk_multiple: I2S_MCLK_MULTIPLE_512 = MCLK = 512 × sample_rate
            //   Result: 48,000 Hz × 512 = 24.576 MHz MCLK
            //   This matches the TX MCLK (192k × 128) for perfect synchronization
            .fixed_mclk = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_512,

            // ---- Channel Bit Width ----
            // I2S_BITS_PER_CHAN_32BIT: 32 bits per channel
            .bits_per_chan = I2S_BITS_PER_CHAN_32BIT
        };

        // ---- Install I2S Driver ----
        //
        // Allocates DMA buffers and configures I2S Port 0 hardware.
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

        i2s_pin_config_t pins = {
            // MCLK: Not configured (shared from TX interface)
            .mck_io_num = I2S_PIN_NO_CHANGE,

            // BCK output: Bit clock (3.072 MHz = 48k × 64)
            .bck_io_num = PIN_ADC_BCK,

            // WS (LRCK) output: Word select (48 kHz)
            .ws_io_num = PIN_ADC_LRCK,

            // Data output: Not used for RX
            .data_out_num = I2S_PIN_NO_CHANGE,

            // Data input: Serial audio data from ADC
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

        // MCLK frequency: 48,000 Hz × 512 = 24.576 MHz (shared from TX)
        log_via_logger_or_serial(LogLevel::INFO, "  MCLK: %.3f MHz (from TX GPIO%d)",
                                 (SAMPLE_RATE_ADC * 512) / 1'000'000.0f, PIN_MCLK);

        // BCK frequency: 48,000 Hz × 64 = 3.072 MHz
        log_via_logger_or_serial(LogLevel::INFO, "  BCK: %.3f MHz (GPIO%d)",
                                 (SAMPLE_RATE_ADC * 64) / 1'000'000.0f, PIN_ADC_BCK);

        // LRCK frequency: Same as sample rate (48 kHz)
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
     *   1. Uninstall TX driver (I2S Port 1)
     *   2. Uninstall RX driver (I2S Port 0)
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

} // namespace AudioIO

// =====================================================================================
//                                END OF FILE
// =====================================================================================
