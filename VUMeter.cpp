/*
 * =====================================================================================
 *
 *                            ESP32 RDS STEREO ENCODER
 *                          VU Meter Implementation
 *
 * =====================================================================================
 *
 * File:         VUMeter.cpp
 * Description:  Real-time VU meter visualization on ILI9341 TFT display
 *
 * Architecture:
 *   • Runs on separate FreeRTOS task (core 1) to avoid blocking audio pipeline
 *   • Receives peak/RMS audio samples via lockless queue from DSP_pipeline
 *   • Uses delta rendering to minimize SPI traffic to display
 *   • Professional VU ballistics with fast attack, slow release
 *
 * Features:
 *   • Dual-channel stereo VU bars with color-coded zones (green/yellow/orange/red)
 *   • Peak hold markers with 1-second hold time
 *   • Linear dB scale (-40 to +3 dB) with proper headroom visualization
 *   • 50 FPS update rate for smooth animation
 *   • Optimized pixel-by-pixel delta rendering
 *
 * Color Zones:
 *   • Green:  0-70%   (Safe operating levels)
 *   • Yellow: 70-85%  (Moderate levels)
 *   • Orange: 85-95%  (High levels)
 *   • Red:    95-100% (Peak/clipping zone, +3dB)
 *
 * Technical Details:
 *   • Display: ILI9341 320x240 @ 40 MHz SPI
 *   • Audio samples: 5ms intervals (200 Hz from DSP_pipeline)
 *   • Visual updates: 20ms intervals (~50 FPS)
 *   • Attack rate: 50 pixels/frame (instant response)
 *   • Release rate: 8 pixels/frame (natural decay)
 *
 * =====================================================================================
 */

#include "VUMeter.h"

#include "Config.h"
#include "Log.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include <freertos/queue.h>
#include <freertos/task.h>

#include <Arduino_GFX_Library.h>

namespace VUMeter
{
    // ==================================================================================
    //                            FREERTOS TASK INFRASTRUCTURE
    // ==================================================================================

    static QueueHandle_t g_queue       = nullptr; // Queue for receiving audio samples
    static QueueHandle_t g_stats_queue = nullptr; // Queue for bottom status snapshot
    static TaskHandle_t  g_task        = nullptr; // VU meter task handle

    // ==================================================================================
    //                            DISPLAY HARDWARE & LAYOUT
    // ==================================================================================

    // Arduino_GFX display driver objects
    static Arduino_DataBus* s_bus = nullptr; // SPI bus interface
    static Arduino_GFX*     s_gfx = nullptr; // ILI9341 graphics driver

    // Display dimensions (ILI9341 in landscape mode)
    static constexpr int DISPLAY_WIDTH  = 320; // Total width in pixels
    static constexpr int DISPLAY_HEIGHT = 240; // Total height in pixels

    // VU meter layout constants
    static constexpr int MARGIN_X       = 16; // Left/right margins
    static constexpr int VU_Y           = 32; // Moved up to leave room for status panel
    static constexpr int VU_WIDTH       = (DISPLAY_WIDTH - 2 * MARGIN_X); // 288px usable width
    static constexpr int VU_LABEL_WIDTH = 14;                          // Width for "L"/"R" labels
    static constexpr int VU_BAR_WIDTH   = (VU_WIDTH - VU_LABEL_WIDTH); // 274px bar width
    static constexpr int VU_BAR_HEIGHT  = 22;                          // Height of each VU bar
    static constexpr int VU_BAR_SPACING = 32; // Vertical space between L and R bars

    // Calculated Y positions for left/right channels
    static constexpr int VU_L_Y = VU_Y; // Left channel Y position
    static constexpr int VU_R_Y = (VU_L_Y + VU_BAR_HEIGHT + VU_BAR_SPACING); // Right channel Y
    static constexpr int MID_SCALE_Y =
        (VU_L_Y + VU_BAR_HEIGHT + (VU_BAR_SPACING / 2)); // dB scale Y

    // Peak marker dimensions
    static constexpr int PEAK_WIDTH  = 3;             // 3-pixel wide white marker
    static constexpr int PEAK_HEIGHT = VU_BAR_HEIGHT; // Full bar height

    // ==================================================================================
    //                              COLOR DEFINITIONS (RGB565)
    // ==================================================================================
    //
    // ILI9341 uses 16-bit RGB565 color format:
    //   • 5 bits red, 6 bits green, 5 bits blue
    //   • Format: RRRRRGGGGGGBBBBB

    static constexpr uint16_t COLOR_BLACK     = 0x0000; // Background
    static constexpr uint16_t COLOR_WHITE     = 0xFFFF; // Peak markers, text
    static constexpr uint16_t COLOR_DARK_GRAY = 0x4208; // Borders, grid lines
    static constexpr uint16_t COLOR_GREEN     = 0x07E0; // Safe levels (0-70%)
    static constexpr uint16_t COLOR_YELLOW    = 0xFFE0; // Moderate (70-85%)
    static constexpr uint16_t COLOR_ORANGE    = 0xFD20; // High levels (85-95%)
    static constexpr uint16_t COLOR_RED       = 0xF800; // Peak/clip (95-100%)

    // ==================================================================================
    //                          COLOR ZONE THRESHOLDS
    // ==================================================================================
    //
    // Returns pixel position where each color zone begins.
    // Based on professional VU meter standards:
    //   • Green:  0-70%   Safe operating range
    //   • Yellow: 70-85%  Approaching optimal
    //   • Orange: 85-95%  High levels, use caution
    //   • Red:    95-100% Peaks and clipping

    static inline int GREEN_TH()
    {
        return (int)roundf(0.70f * VU_BAR_WIDTH); // 70% of bar = 192 pixels
    }

    static inline int YELLOW_TH()
    {
        return (int)roundf(0.85f * VU_BAR_WIDTH); // 85% of bar = 233 pixels
    }

    static inline int RED_TH()
    {
        return (int)roundf(0.95f * VU_BAR_WIDTH); // 95% of bar = 260 pixels
    }

    // ==================================================================================
    //                            UTILITY FUNCTIONS
    // ==================================================================================

    /**
     * Clamp integer value to range [lo, hi]
     * @param v   Value to clamp
     * @param lo  Minimum value
     * @param hi  Maximum value
     * @return    Clamped value
     */
    static inline int clampi(int v, int lo, int hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // ==================================================================================
    //                       dB TO PIXEL MAPPING (VU VALUES)
    // ==================================================================================

    /**
     * Convert dB value to pixel position for VU bar rendering
     *
     * This function implements professional VU meter scaling where:
     *   • 0 dBFS appears at ~93% of scale (yellow/orange zone)
     *   • +3 dBFS (clipping) reaches 100% (red zone)
     *   • Linear dB scale provides accurate level representation
     *
     * Mapping Table:
     *   -40 dBFS →   0% → 0 pixels   (silence)
     *   -30 dBFS →  23% → 63 pixels  (low)
     *   -20 dBFS →  47% → 128 pixels (moderate)
     *   -12 dBFS →  65% → 178 pixels (good level)
     *    -6 dBFS →  79% → 216 pixels (high)
     *    -3 dBFS →  86% → 235 pixels (very high)
     *     0 dBFS →  93% → 254 pixels (peak)
     *    +3 dBFS → 100% → 274 pixels (clipping/red zone)
     *
     * @param dB  Input level in dBFS (after applying VU_DB_OFFSET)
     * @return    Pixel position (0 to VU_BAR_WIDTH)
     */
    static int dbToX(float dB)
    {
        // Define the display range to include headroom for +3dB red zone
        const float DB_MIN = -40.0f; // Minimum dB to display (below = silent)
        const float DB_MAX = 3.0f;   // Maximum dB (+3dB = full scale red zone)

        // Clamp input to display range
        dB = std::max(DB_MIN, std::min(DB_MAX, dB));

        // Linear normalization: -40dB→0.0, +3dB→1.0
        // 43 dB total range (professional VU meter standard)
        float normalized = (dB - DB_MIN) / (DB_MAX - DB_MIN);

        // Convert to pixels with rounding
        int px = static_cast<int>(normalized * VU_BAR_WIDTH + 0.5f);

        // Final safety clamp
        return clampi(px, 0, VU_BAR_WIDTH);
    }

    static int dbToX_Scale(float dB)
    {
        // Map scale label dB values to pixel positions
        // This is for drawing the dB scale, not for VU meter values
        // Scale labels represent the actual dB values on the meter face
        // Uses simple linear mapping matching the VU meter range

        const float SCALE_MIN = -20.0f; // Left edge of scale
        const float SCALE_MAX = 3.0f;   // Right edge of scale (+3dB red zone)

        // Clamp to scale range
        dB = std::max(SCALE_MIN, std::min(SCALE_MAX, dB));

        // Simple linear mapping
        float normalized = (dB - SCALE_MIN) / (SCALE_MAX - SCALE_MIN);

        // Scale to pixel width
        int px = static_cast<int>(normalized * VU_BAR_WIDTH + 0.5f);
        return clampi(px, 0, VU_BAR_WIDTH);
    }

    static void drawScale()
    {
        // Clear bar area
        s_gfx->fillRect(MARGIN_X, VU_L_Y, VU_WIDTH, (VU_BAR_HEIGHT * 2 + VU_BAR_SPACING), 0x0000);

        // Bar labels (L/R)
        s_gfx->setTextWrap(false);
        s_gfx->setTextColor(0xFFFF);
        s_gfx->setTextSize(1);
        s_gfx->setCursor(MARGIN_X, VU_L_Y + VU_BAR_HEIGHT - 12);
        s_gfx->print("L");
        s_gfx->setCursor(MARGIN_X, VU_R_Y + VU_BAR_HEIGHT - 12);
        s_gfx->print("R");

        // Vertical grid
        const int gridTicks = 5;
        for (int i = 0; i <= gridTicks; i++)
        {
            int x = MARGIN_X + VU_LABEL_WIDTH + (i * VU_BAR_WIDTH) / gridTicks;
            s_gfx->drawFastVLine(x, VU_L_Y - 2, VU_BAR_HEIGHT * 2 + VU_BAR_SPACING + 4, 0x4208);
        }

        // Mid dB scale band
        int x0    = MARGIN_X + VU_LABEL_WIDTH;
        int bandY = MID_SCALE_Y - 12;
        int bandH = 24;
        s_gfx->fillRect(x0, bandY, VU_BAR_WIDTH, bandH, 0x0000);
        s_gfx->drawFastHLine(x0, MID_SCALE_Y, VU_BAR_WIDTH, 0x7BEF);

        // Labels
        const float labels[] = {-20, -10, -6, -3, -1, 0, 3};
        const int   nLabels  = sizeof(labels) / sizeof(labels[0]);
        for (int i = 0; i < nLabels; ++i)
        {
            int px = x0 + dbToX_Scale(labels[i]);
            s_gfx->drawFastVLine(px, MID_SCALE_Y - 8, 16, 0xFFFF);
            char buf[8];
            if (labels[i] == 0)
                snprintf(buf, sizeof(buf), "0");
            else if (labels[i] > 0)
                snprintf(buf, sizeof(buf), "+%d", (int)labels[i]);
            else
                snprintf(buf, sizeof(buf), "%d", (int)labels[i]);
            int approx_w = 12 * strlen(buf);
            int text_x   = px - approx_w / 2;
            int text_y   = MID_SCALE_Y - 8;
            s_gfx->setCursor(text_x, text_y);
            s_gfx->print(buf);
        }

        // Units
        s_gfx->setCursor(x0 + VU_BAR_WIDTH + 4, MID_SCALE_Y - 18);
        s_gfx->print("dB");
    }

    struct Channel
    {
        int      avg        = 0;
        int      peak       = -1;
        uint32_t peakExpiry = 0;
        int      y          = 0;
        int      target     = 0;
    };

    static Channel chL{0, -1, 0, VU_L_Y, 0};
    static Channel chR{0, -1, 0, VU_R_Y, 0};

    static uint32_t           nextDecayAt       = 0;
    static constexpr int      ATTACK_STEP       = 50; // Very fast attack for instant response
    static constexpr int      RELEASE_STEP      = 8;  // Slightly faster release
    static constexpr int      DECAY_INTERVAL_MS = 16; // Match display frame rate
    static constexpr uint32_t PEAK_HOLD_MS      = 1000;

    static inline uint16_t vuColorAt(int pos /*0..VU_BAR_WIDTH-1*/)
    {
        if (pos < GREEN_TH())
            return COLOR_GREEN;
        if (pos < YELLOW_TH())
            return COLOR_YELLOW;
        if (pos < RED_TH())
            return COLOR_ORANGE;
        return COLOR_RED;
    }

    // Optimized bar redraw: only update changed segments and maintain a clean outline
    static void drawVUBarDelta(Channel& ch, int newLen, int newPeak, int prevLen, int prevPeak)
    {
        const int barX     = MARGIN_X + VU_LABEL_WIDTH;
        const int barY     = ch.y;
        const int innerTop = barY + 2;
        const int innerH   = VU_BAR_HEIGHT - 4;

        // First-time draw: outline and clear inner area
        if (prevLen < 0)
        {
            s_gfx->fillRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2, COLOR_BLACK);
            s_gfx->drawRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2,
                            COLOR_DARK_GRAY);
            s_gfx->fillRect(barX, innerTop, VU_BAR_WIDTH, innerH, COLOR_BLACK);
        }

        // ===== PEAK MARKER CLEANUP FIRST =====
        // Restore old peak marker area with appropriate bar color BEFORE drawing new bar
        // This prevents black gaps when bar grows through old peak position
        if (prevPeak >= 0 && prevPeak != newPeak)
        {
            int oldPeakX = barX + prevPeak;

            // If old peak is within the new bar length, restore with bar color
            if (prevPeak < newLen)
            {
                // Restore the area under old peak with correct gradient colors
                for (int x = prevPeak; x < prevPeak + PEAK_WIDTH && x < newLen; x++)
                {
                    uint16_t color = vuColorAt(x);
                    s_gfx->drawFastVLine(barX + x, innerTop, innerH, color);
                }
            }
            // If old peak is beyond new bar length, clear with black
            else
            {
                s_gfx->fillRect(oldPeakX, innerTop, PEAK_WIDTH, innerH, COLOR_BLACK);
            }
        }

        // ===== BAR UPDATES =====
        // If bar got shorter, clear the area that's no longer filled
        if (newLen < prevLen)
        {
            int clearX     = barX + newLen;
            int clearWidth = prevLen - newLen;
            s_gfx->fillRect(clearX, innerTop, clearWidth, innerH, COLOR_BLACK);
        }

        // Draw bar segments (only the new/changed portion)
        if (newLen > 0)
        {
            int startX = 0;
            int endX   = newLen;

            // If this is an incremental update, only draw the new part
            if (prevLen > 0 && newLen > prevLen)
            {
                startX = prevLen;
            }
            // If shrinking or first time, draw the whole bar
            else if (prevLen < 0 || newLen <= prevLen)
            {
                startX = 0;
            }

            // Draw pixel by pixel with appropriate colors
            for (int x = startX; x < endX; x++)
            {
                uint16_t color = vuColorAt(x);
                s_gfx->drawFastVLine(barX + x, innerTop, innerH, color);
            }
        }

        // ===== PEAK MARKER DRAWING =====
        // Draw new peak marker at current position (always on top)
        if (newPeak >= 0 && newPeak < VU_BAR_WIDTH)
        {
            int peakX = barX + newPeak;
            s_gfx->fillRect(peakX, innerTop, PEAK_WIDTH, innerH, COLOR_WHITE);
        }

        // ===== BAR OUTLINE (redraw for definition) =====
        s_gfx->drawRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2, COLOR_DARK_GRAY);
    }

    static void updateBar(Channel& ch, int& prevLen, int& prevPeak)
    {
        // Apply fast attack: move bar towards target
        int target = clampi(ch.target, 0, VU_BAR_WIDTH);
        if (target > ch.avg)
        {
            int delta = target - ch.avg;
            int step  = (delta > ATTACK_STEP) ? ATTACK_STEP : delta;
            ch.avg    = ch.avg + step;
        }

        // Update peak marker based on the SMOOTHED bar position (ch.avg), not raw target
        uint32_t now = millis();
        if (ch.avg - 1 > ch.peak) // -1 ensures peak stays ahead of bar
        {
            ch.peak = ch.avg - 1;
            if (ch.peak < 0)
                ch.peak = -1; // Hide if bar is at zero
            ch.peakExpiry = now + PEAK_HOLD_MS;
        }
        else if (ch.peak >= 0 && now >= ch.peakExpiry && ch.avg <= ch.peak)
        {
            ch.peak = -1; // Clear peak marker
        }

        // Draw only the delta for the bar and peak, passing previous values
        drawVUBarDelta(ch, ch.avg, ch.peak, prevLen, prevPeak);

        // Update cached values for next frame
        prevLen  = ch.avg;
        prevPeak = ch.peak;
    }

    static void decayIfDue()
    {
        uint32_t now = millis();
        if ((int32_t)(now - nextDecayAt) < 0)
            return;
        nextDecayAt = now + DECAY_INTERVAL_MS;
        if (chL.avg > 0)
            chL.avg = std::max(0, chL.avg - RELEASE_STEP);
        if (chR.avg > 0)
            chR.avg = std::max(0, chR.avg - RELEASE_STEP);
    }

    static void vu_task(void* arg)
    {
        (void)arg;
        Sample        s;
        StatsSnapshot stats; // receives bottom-panel status snapshots

        bool display_ok = false;
        if (Config::VU_DISPLAY_ENABLED)
        {
            // Optional BL pin
            if (Config::TFT_BL >= 0)
            {
                pinMode((int)Config::TFT_BL, OUTPUT);
                digitalWrite((int)Config::TFT_BL, HIGH);
            }

            s_bus = new Arduino_ESP32SPI(Config::TFT_DC, Config::TFT_CS,
                                         Config::TFT_SCK, Config::TFT_MOSI,
                                         GFX_NOT_DEFINED /* MISO */, 1 /* spi_num */);
            s_gfx = new Arduino_ILI9341(s_bus, Config::TFT_RST, Config::TFT_ROTATION,
                                        false /*IPS*/);
            if (s_gfx && s_gfx->begin())
            {
                s_gfx->fillScreen(0x0000);
                s_gfx->setTextWrap(false);
                s_gfx->setTextColor(0xFFFF);
                s_gfx->setTextSize(2);
                s_gfx->setCursor(15, 10);
                s_gfx->print("ESP32 RDS STEREO ENCODER");
                drawScale();
                nextDecayAt = millis() + DECAY_INTERVAL_MS;
                display_ok  = true;
                Log::enqueue(Log::INFO, "VU display initialized (ILI9341)");
            }
            else
            {
                Log::enqueue(Log::WARN, "VU display init failed; falling back to ASCII");
            }
        }

        // For decay ticking when queue is quiet
        TickType_t wait_ticks = pdMS_TO_TICKS(10);

        uint32_t       last_frame_ms     = millis();
        const uint32_t FRAME_INTERVAL_MS = 20; // ~60 FPS pacing to avoid flicker

        // Previous frame state for change detection
        int prevLenL  = -1;
        int prevLenR  = -1;
        int prevPeakL = -1;
        int prevPeakR = -1;

        for (;;)
        {
            if (xQueueReceive(g_queue, &s, wait_ticks) == pdTRUE)
            {
                float l = std::isfinite(s.l_dbfs) ? s.l_dbfs : -120.0f;
                float r = std::isfinite(s.r_dbfs) ? s.r_dbfs : -120.0f;

                if (Config::VU_DISPLAY_ENABLED && display_ok)
                {
                    // Use peak dBFS directly with offset, no additional smoothing
                    // Attack/release ballistics are handled by updateBar()
                    // The offset shifts the scale to make typical audio levels more visible
                    float ldb = l + Config::VU_DB_OFFSET;
                    float rdb = r + Config::VU_DB_OFFSET;

                    // dbToX handles clamping internally
                    chL.target = dbToX(ldb);
                    chR.target = dbToX(rdb);
                }
            }

            if (Config::VU_DISPLAY_ENABLED && display_ok)
            {
                uint32_t now_ms = millis();
                if ((int32_t)(now_ms - last_frame_ms) >= (int32_t)FRAME_INTERVAL_MS)
                {
                    last_frame_ms = now_ms;

                    updateBar(chL, prevLenL, prevPeakL);
                    updateBar(chR, prevLenR, prevPeakR);
                    decayIfDue();
                }

                // Draw status panel when a new snapshot arrives (non-blocking)
                if (g_stats_queue && xQueueReceive(g_stats_queue, &stats, 0) == pdTRUE)
                {
                    const int panelY = DISPLAY_HEIGHT - Config::STATUS_PANEL_HEIGHT;
                    s_gfx->fillRect(0, panelY, DISPLAY_WIDTH, Config::STATUS_PANEL_HEIGHT,
                                    COLOR_BLACK);
                    s_gfx->setTextWrap(false);
                    s_gfx->setTextColor(COLOR_WHITE);
                    s_gfx->setTextSize(1);

                    char line[64];
                    int  y = panelY + 4;

                    if (stats.cpu_valid)
                    {
                        snprintf(line, sizeof(line), "Cores: Core0 %.1f%%  Core1 %.1f%%",
                                 (double)stats.core0_load, (double)stats.core1_load);
                    }
                    else
                    {
                        snprintf(line, sizeof(line), "Cores: n/a (enable run-time stats)");
                    }
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);
                    y += 12;

                    snprintf(line, sizeof(line), "Audio proc: %.1f us (min %.1f, max %.1f)",
                             (double)stats.total_us_cur, (double)stats.total_us_min,
                             (double)stats.total_us_max);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);
                    y += 12;

                    if (stats.cpu_valid)
                    {
                        // Separate tasks line
                        snprintf(line, sizeof(line), "Tasks: Aud %.1f%%  Log %.1f%%  VU %.1f%%",
                                 (double)stats.audio_cpu, (double)stats.logger_cpu,
                                 (double)stats.vu_cpu);
                        s_gfx->setCursor(4, y);
                        s_gfx->print(line);
                        y += 12;
                    }
                    // Separate stages lines (two per line for readability)
                    snprintf(line, sizeof(line), "Stages: FIR %.1f us  MPX %.1f us",
                             (double)stats.fir_us_cur, (double)stats.mpx_us_cur);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);
                    y += 12;
                    snprintf(line, sizeof(line), "Stages: Mat %.1f us  RDS %.1f us",
                             (double)stats.matrix_us_cur, (double)stats.rds_us_cur);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);
                    y += 12;

                    snprintf(line, sizeof(line), "Heap: %u KB (min %u)  Uptime: %u s",
                             (unsigned)(stats.heap_free / 1024u),
                             (unsigned)(stats.heap_min / 1024u), (unsigned)stats.uptime_s);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);

                    // Additional lines using available vertical space
                    y += 12;
                    // Line: Sample rates and configuration
                    snprintf(
                        line, sizeof(line), "Rates: %u kHz -> %u kHz  Up %ux  Block %u  %u-bit",
                        (unsigned)(Config::SAMPLE_RATE_ADC / 1000u),
                        (unsigned)(Config::SAMPLE_RATE_DAC / 1000u),
                        (unsigned)Config::UPSAMPLE_FACTOR, (unsigned)Config::BLOCK_SIZE,
                        (unsigned)Config::BITS_PER_SAMPLE);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);

                    // Next line: Loops and errors
                    y += 12;
                    snprintf(line, sizeof(line), "Loops: %u  Errors: %u",
                             (unsigned)stats.loops_completed, (unsigned)stats.errors);
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);

                    // Next line: Stack min free (KB)
                    y += 12;
                    unsigned a_kb = (stats.audio_stack_free_words * 4u) / 1024u;
                    unsigned l_kb = (stats.logger_stack_free_words * 4u) / 1024u;
                    unsigned v_kb = (stats.vu_stack_free_words * 4u) / 1024u;
                    if (stats.cpu_valid)
                    {
                        snprintf(line, sizeof(line),
                                 "Stacks min free: Aud %u KB  Log %u KB  VU %u KB", a_kb, l_kb,
                                 v_kb);
                    }
                    else
                    {
                        snprintf(line, sizeof(line),
                                 "Stacks (min free): n/a (enable run-time stats)");
                    }
                    s_gfx->setCursor(4, y);
                    s_gfx->print(line);
                }
            }
        }
    }

    bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words, std::size_t queue_len)
    {
        if (g_queue || g_task)
        {
            return true; // already running
        }

        if (queue_len == 0)
        {
            queue_len = 1;
        }
        g_queue       = xQueueCreate((UBaseType_t)queue_len, sizeof(Sample));
        g_stats_queue = xQueueCreate(1, sizeof(StatsSnapshot));
        if (!g_queue)
        {
            return false;
        }

        BaseType_t ok = xTaskCreatePinnedToCore(vu_task, "vu", stack_words, nullptr, priority,
                                                &g_task, core_id);
        return ok == pdPASS;
    }

    void stopTask()
    {
        if (g_task)
        {
            vTaskDelete(g_task);
            g_task = nullptr;
        }
        if (g_queue)
        {
            vQueueDelete(g_queue);
            g_queue = nullptr;
        }
    }

    bool enqueue(const Sample& s)
    {
        if (!g_queue)
        {
            return false;
        }
        if (uxQueueSpacesAvailable(g_queue) == 0 && uxQueueMessagesWaiting(g_queue) == 1)
        {
            xQueueOverwrite(g_queue, &s);
            return true;
        }
        return xQueueSend(g_queue, &s, 0) == pdTRUE;
    }

    bool enqueueFromISR(const Sample& s, BaseType_t* pxHigherPriorityTaskWoken)
    {
        if (!g_queue)
        {
            return false;
        }
        BaseType_t x = xQueueSendFromISR(g_queue, &s, pxHigherPriorityTaskWoken);
        if (x != pdTRUE)
        {
#if (INCLUDE_xQueueOverwriteFromISR == 1)
            x = xQueueOverwriteFromISR(g_queue, &s, pxHigherPriorityTaskWoken);
            return x == pdTRUE;
#else
            return false;
#endif
        }
        return true;
    }

    bool enqueueStats(const StatsSnapshot& s)
    {
        if (!g_stats_queue)
        {
            return false;
        }
        if (uxQueueSpacesAvailable(g_stats_queue) == 0 &&
            uxQueueMessagesWaiting(g_stats_queue) == 1)
        {
            xQueueOverwrite(g_stats_queue, &s);
            return true;
        }
        return xQueueSend(g_stats_queue, &s, 0) == pdTRUE;
    }
} // namespace VUMeter
