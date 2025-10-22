/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                          Display Manager (ModuleBase)
 *
 * =====================================================================================
 *
 * File:         DisplayManager.cpp
 * Description:  Display manager handling VU visualization and RT/PS UI on ILI9341
 *
 * Architecture:
 *   * Runs on separate FreeRTOS task (core 1) to avoid blocking audio pipeline
 *   * Receives peak/RMS audio samples via lockless queue from DSP_pipeline
 *   * Uses delta rendering to minimize SPI traffic to display
 *   * Professional VU ballistics with fast attack, slow release
 *
 * Features:
 *   * Dual-channel stereo VU bars with color-coded zones (green/yellow/orange/red)
 *   * Peak hold markers with 1-second hold time
 *   * Linear dB scale (-40 to +3 dB) with proper headroom visualization
 *   * 50 FPS update rate for smooth animation
 *   * Optimized pixel-by-pixel delta rendering
 *
 * Color Zones:
 *   * Green:  0-70%   (Safe operating levels)
 *   * Yellow: 70-85%  (Moderate levels)
 *   * Orange: 85-95%  (High levels)
 *   * Red:    95-100% (Peak/clipping zone, +3dB)
 *
 * Technical Details:
 *   * Display: ILI9341 320x240 at 40 MHz SPI
 *   * Audio samples: 5ms intervals (200 Hz from DSP_pipeline)
 *   * Visual updates: 20ms intervals (~50 FPS)
 *   * Attack rate: 50 pixels/frame (instant response)
 *   * Release rate: 8 pixels/frame (natural decay)
 *
 * =====================================================================================
 */

#include "DisplayManager.h"

#include "Config.h"
#include "Console.h"
#include "DSP_pipeline.h"
#include "RDSAssembler.h"
#include "PtyMap.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include <freertos/queue.h>
#include <freertos/task.h>

#include <Arduino_GFX_Library.h>

// ----------------------------------------------------------------------------------
//                      Shared UI Layout Constants (file-scope)
// ----------------------------------------------------------------------------------
namespace {
// Global vertical shift applied to the whole main screen UI (negative moves up)
constexpr int UI_SHIFT_Y = -10; // raise UI by 10 px
constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;
constexpr int MARGIN_X = 16;
constexpr int VU_BAR_HEIGHT = 22;
constexpr int VU_BAR_SPACING = 32;
constexpr int BOTTOM_MARGIN = 8;
constexpr int VU_Y = DISPLAY_HEIGHT - (2 * VU_BAR_HEIGHT + VU_BAR_SPACING) - BOTTOM_MARGIN + UI_SHIFT_Y;
constexpr int VU_WIDTH = (DISPLAY_WIDTH - 2 * MARGIN_X);
constexpr int VU_LABEL_WIDTH = 14;
constexpr int VU_BAR_WIDTH = (VU_WIDTH - VU_LABEL_WIDTH);
constexpr int VU_L_Y = VU_Y;
constexpr int VU_R_Y = (VU_L_Y + VU_BAR_HEIGHT + VU_BAR_SPACING);
constexpr int MID_SCALE_Y = (VU_L_Y + VU_BAR_HEIGHT + (VU_BAR_SPACING / 2));
constexpr int PEAK_WIDTH = 3;
constexpr int PEAK_HEIGHT = VU_BAR_HEIGHT;
// Shared top text/status layout
constexpr int STATUS_Y = 28 + UI_SHIFT_Y;   // small area above PS line
constexpr int DIVIDER_PS_Y = 50 + UI_SHIFT_Y;   // divider under PS
constexpr int DIVIDER_ABOVE_VU_Y = 138 + UI_SHIFT_Y; // divider above VU
constexpr int TEXT_PS_Y = 70 + UI_SHIFT_Y;    // Program Service text baseline
} // namespace

// ==================================================================================
//                          SINGLETON INSTANCE
// ==================================================================================

DisplayManager &DisplayManager::getInstance()
{
    static DisplayManager s_instance;
    return s_instance;
}

// ----------------------------------------------------------------------------------
//                      UI Marquee Text (Long-form RT for Display)
// ----------------------------------------------------------------------------------
// Independent of the 64-char RDS RT, the UI can present a much longer RT string.
// This buffer is set via DisplayManager::setDisplayRT() and consumed by the process loop.
static char s_ui_rt_long[1024] = {0};

void DisplayManager::setDisplayRT(const char *rt_long)
{
    if (!rt_long)
    {
        s_ui_rt_long[0] = '\0';
        return;
    }
    strncpy(s_ui_rt_long, rt_long, sizeof(s_ui_rt_long) - 1);
    s_ui_rt_long[sizeof(s_ui_rt_long) - 1] = '\0';
}

// ==================================================================================
//                          CONSTRUCTOR & MEMBER INITIALIZATION
// ==================================================================================

DisplayManager::DisplayManager()
    : queue_(nullptr), stats_queue_(nullptr), queue_len_(1), core_id_(1), priority_(1),
      stack_words_(4096), sample_overflow_count_(0), stats_overflow_count_(0),
      sample_overflow_logged_(false)
{
    // All members initialized via initializer list
}

// ==================================================================================
//                          STATIC WRAPPER API
// ==================================================================================

bool DisplayManager::startTask(int core_id, UBaseType_t priority, uint32_t stack_words,
                               size_t queue_len)
{
    DisplayManager &vu = getInstance();
    vu.queue_len_ = queue_len;
    if (vu.queue_len_ == 0)
        vu.queue_len_ = 1;
    vu.core_id_ = core_id;
    vu.priority_ = priority;
    vu.stack_words_ = stack_words;

    // Spawn display task via ModuleBase helper
    return vu.spawnTask("vu", (uint32_t)stack_words, priority, core_id,
                        DisplayManager::taskTrampoline);
}

void DisplayManager::stopTask()
{
    DisplayManager &vu = getInstance();
    if (vu.isRunning())
    {
        TaskHandle_t handle = vu.getTaskHandle();
        if (handle)
        {
            vTaskDelete(handle);
            vu.setTaskHandle(nullptr);
        }
    }
}

bool DisplayManager::isReady()
{
    return getInstance().isRunning();
}

bool DisplayManager::enqueue(const VUSample &s)
{
    return getInstance().enqueueRaw(s);
}

bool DisplayManager::enqueueFromISR(const VUSample &s, BaseType_t *pxHigherPriorityTaskWoken)
{
    return getInstance().enqueueFromISRRaw(s, pxHigherPriorityTaskWoken);
}

bool DisplayManager::enqueueStats(const VUStatsSnapshot &s)
{
    return getInstance().enqueueStatsRaw(s);
}

// ==================================================================================
//                          MODULEBASE IMPLEMENTATION
// ==================================================================================

void DisplayManager::taskTrampoline(void *arg)
{
    ModuleBase::defaultTaskTrampoline(arg);
}

bool DisplayManager::begin()
{
    // Create FreeRTOS queues
    queue_ = xQueueCreate((UBaseType_t)queue_len_, sizeof(VUSample));
    stats_queue_ = xQueueCreate(1, sizeof(VUStatsSnapshot));

    if (!queue_)
    {
        ErrorHandler::logError(ErrorCode::INIT_QUEUE_FAILED, "DisplayManager::begin",
                               "sample queue creation failed");
        return false;
    }

    if (!stats_queue_)
    {
        ErrorHandler::logError(ErrorCode::INIT_QUEUE_FAILED, "DisplayManager::begin",
                               "stats queue creation failed");
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }

    // Initialize display (optional)
    if (Config::VU_DISPLAY_ENABLED)
    {
        Console::enqueuef(LogLevel::INFO, "DisplayManager running on Core %d", xPortGetCoreID());
        if (Config::TFT_BL >= 0)
        {
            pinMode((int)Config::TFT_BL, OUTPUT);
            digitalWrite((int)Config::TFT_BL, HIGH);
        }

        // Create bus and GFX objects
        bus_ = new Arduino_ESP32SPI(Config::TFT_DC, Config::TFT_CS, Config::TFT_SCK,
                                    Config::TFT_MOSI, GFX_NOT_DEFINED, 1);
        gfx_ = new Arduino_ILI9341(bus_, Config::TFT_RST, Config::TFT_ROTATION, false);

        if (gfx_ && gfx_->begin())
        {
            gfx_->fillScreen(Config::UI::COLOR_BG);
            gfx_->setTextWrap(false);
            gfx_->setTextColor(0xFFFF);
            // Removed top static title line; splash screen now provides branding

            // Draw static scale
            auto drawScale = [&]()
            {
                gfx_->fillRect(MARGIN_X, VU_L_Y, VU_WIDTH, (VU_BAR_HEIGHT * 2 + VU_BAR_SPACING),
                               0x0000);

                gfx_->setTextWrap(false);
                gfx_->setTextColor(0xFFFF);
                gfx_->setTextSize(1);
                gfx_->setCursor(MARGIN_X, VU_L_Y + VU_BAR_HEIGHT - 12);
                gfx_->print("L");
                gfx_->setCursor(MARGIN_X, VU_R_Y + VU_BAR_HEIGHT - 12);
                gfx_->print("R");

                const int gridTicks = 5;
                for (int i = 0; i <= gridTicks; i++)
                {
                    int x = MARGIN_X + VU_LABEL_WIDTH + (i * VU_BAR_WIDTH) / gridTicks;
                    gfx_->drawFastVLine(x, VU_L_Y - 2, VU_BAR_HEIGHT * 2 + VU_BAR_SPACING + 4,
                                        0x4208);
                }

                int x0 = MARGIN_X + VU_LABEL_WIDTH;
                int bandY = MID_SCALE_Y - 12;
                int bandH = 24;
                gfx_->fillRect(x0, bandY, VU_BAR_WIDTH, bandH, 0x0000);
                gfx_->drawFastHLine(x0, MID_SCALE_Y, VU_BAR_WIDTH, 0x7BEF);

                auto dbToX_Scale = [&](float dB)
                {
                    const float SCALE_MIN = -20.0f;
                    const float SCALE_MAX = 3.0f;
                    dB = std::max(SCALE_MIN, std::min(SCALE_MAX, dB));
                    float normalized = (dB - SCALE_MIN) / (SCALE_MAX - SCALE_MIN);
                    int px = static_cast<int>(normalized * VU_BAR_WIDTH + 0.5f);
                    px = px < 0 ? 0 : (px > VU_BAR_WIDTH ? VU_BAR_WIDTH : px);
                    return px;
                };

                const float labels[] = {-20, -10, -6, -3, -1, 0, 3};
                const int nLabels = sizeof(labels) / sizeof(labels[0]);
                for (int i = 0; i < nLabels; ++i)
                {
                    int px = x0 + dbToX_Scale(labels[i]);
                    gfx_->drawFastVLine(px, MID_SCALE_Y - 8, 16, 0xFFFF);
                    char buf[8];
                    if (labels[i] == 0)
                        snprintf(buf, sizeof(buf), "0");
                    else if (labels[i] > 0)
                        snprintf(buf, sizeof(buf), "+%d", (int)labels[i]);
                    else
                        snprintf(buf, sizeof(buf), "%d", (int)labels[i]);
                    int approx_w = 12 * strlen(buf);
                    int text_x = px - approx_w / 2;
                    int text_y = MID_SCALE_Y - 4;
                    gfx_->setCursor(text_x, text_y);
                    gfx_->print(buf);
                }

                gfx_->setCursor(x0 + VU_BAR_WIDTH + 4, MID_SCALE_Y - 4);
                gfx_->print("dB");
            };

            drawScale();
            // Static accent dividers (match layout constants used in process())
            {
                gfx_->drawFastHLine(MARGIN_X, DIVIDER_PS_Y, VU_WIDTH, Config::UI::COLOR_ACCENT);  // under PS
                gfx_->drawFastHLine(MARGIN_X, DIVIDER_ABOVE_VU_Y, VU_WIDTH, Config::UI::COLOR_ACCENT); // above VU
            }
            ErrorHandler::logInfo("DisplayManager", "VU display initialized (ILI9341)");
        }
        else
        {
            ErrorHandler::logWarning("DisplayManager",
                                     "VU display init failed; falling back to ASCII");
        }
    }

    ErrorHandler::logInfo("DisplayManager", "Task initialized successfully");
    return true;
}

void DisplayManager::process()
{
    // ==================================================================================
    //                      DISPLAY HARDWARE & LAYOUT
    // ==================================================================================

    // ==================================================================================
    //                              COLOR DEFINITIONS (RGB565)
    // ==================================================================================

    static constexpr uint16_t COLOR_BLACK = 0x0000;
    static constexpr uint16_t COLOR_WHITE = 0xFFFF;
    static constexpr uint16_t COLOR_DARK_GRAY = 0x4208;
    static constexpr uint16_t COLOR_GREEN = 0x07E0;
    static constexpr uint16_t COLOR_YELLOW = 0xFFE0;
    static constexpr uint16_t COLOR_ORANGE = 0xFD20;
    static constexpr uint16_t COLOR_RED = 0xF800;

    // ==================================================================================
    //                          COLOR ZONE THRESHOLDS
    // ==================================================================================

    auto GREEN_TH = []()
    {
        return (int)roundf(0.70f * VU_BAR_WIDTH);
    };
    auto YELLOW_TH = []()
    {
        return (int)roundf(0.85f * VU_BAR_WIDTH);
    };
    auto RED_TH = []()
    {
        return (int)roundf(0.95f * VU_BAR_WIDTH);
    };

    // ==================================================================================
    //                            UTILITY FUNCTIONS
    // ==================================================================================

    auto clampi = [](int v, int lo, int hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    };

    // ==================================================================================
    //                       dB TO PIXEL MAPPING (VU VALUES)
    // ==================================================================================

    auto dbToX = [&clampi](float dB)
    {
        const float DB_MIN = -40.0f;
        const float DB_MAX = 3.0f;
        dB = std::max(DB_MIN, std::min(DB_MAX, dB));
        float normalized = (dB - DB_MIN) / (DB_MAX - DB_MIN);
        int px = static_cast<int>(normalized * VU_BAR_WIDTH + 0.5f);
        return clampi(px, 0, VU_BAR_WIDTH);
    };

    auto dbToX_Scale = [&clampi](float dB)
    {
        const float SCALE_MIN = -20.0f;
        const float SCALE_MAX = 3.0f;
        dB = std::max(SCALE_MIN, std::min(SCALE_MAX, dB));
        float normalized = (dB - SCALE_MIN) / (SCALE_MAX - SCALE_MIN);
        int px = static_cast<int>(normalized * VU_BAR_WIDTH + 0.5f);
        return clampi(px, 0, VU_BAR_WIDTH);
    };

    // ==================================================================================
    //                          DRAWING FUNCTIONS
    // ==================================================================================

    // ==================================================================================
    //                          CHANNEL STATE
    // ==================================================================================

    struct Channel
    {
        int avg = 0;
        int peak = -1;
        uint32_t peakExpiry = 0;
        int y = 0;
        int target = 0;
    };

    static Channel chL{0, -1, 0, VU_L_Y, 0};
    static Channel chR{0, -1, 0, VU_R_Y, 0};
    static uint32_t nextDecayAt = 0;

    static constexpr int ATTACK_STEP = 50;
    static constexpr int RELEASE_STEP = 8;
    static constexpr int DECAY_INTERVAL_MS = 16;
    static constexpr uint32_t PEAK_HOLD_MS = 1000;

    auto vuColorAt = [&GREEN_TH, &YELLOW_TH, &RED_TH](int pos) -> uint16_t
    {
        if (pos < GREEN_TH())
            return COLOR_GREEN;
        if (pos < YELLOW_TH())
            return COLOR_YELLOW;
        if (pos < RED_TH())
            return COLOR_ORANGE;
        return COLOR_RED;
    };

    auto drawVUBarDelta = [&](Channel &ch, int newLen, int newPeak, int prevLen, int prevPeak)
    {
        const int barX = MARGIN_X + VU_LABEL_WIDTH;
        const int barY = ch.y;
        const int innerTop = barY + 2;
        const int innerH = VU_BAR_HEIGHT - 4;

        if (prevLen < 0)
        {
            gfx_->fillRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2, COLOR_BLACK);
            gfx_->drawRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2,
                           COLOR_DARK_GRAY);
            gfx_->fillRect(barX, innerTop, VU_BAR_WIDTH, innerH, COLOR_BLACK);
        }

        if (prevPeak >= 0 && prevPeak != newPeak)
        {
            int oldPeakX = barX + prevPeak;
            if (prevPeak < newLen)
            {
                for (int x = prevPeak; x < prevPeak + PEAK_WIDTH && x < newLen; x++)
                {
                    uint16_t color = vuColorAt(x);
                    gfx_->drawFastVLine(barX + x, innerTop, innerH, color);
                }
            }
            else
            {
                gfx_->fillRect(oldPeakX, innerTop, PEAK_WIDTH, innerH, COLOR_BLACK);
            }
        }

        if (newLen < prevLen)
        {
            int clearX = barX + newLen;
            int clearWidth = prevLen - newLen;
            gfx_->fillRect(clearX, innerTop, clearWidth, innerH, COLOR_BLACK);
        }

        if (newLen > 0)
        {
            int startX = 0;
            int endX = newLen;

            if (prevLen > 0 && newLen > prevLen)
                startX = prevLen;
            else if (prevLen < 0 || newLen <= prevLen)
                startX = 0;

            for (int x = startX; x < endX; x++)
            {
                uint16_t color = vuColorAt(x);
                gfx_->drawFastVLine(barX + x, innerTop, innerH, color);
            }
        }

        if (newPeak >= 0 && newPeak < VU_BAR_WIDTH)
        {
            int peakX = barX + newPeak;
            gfx_->fillRect(peakX, innerTop, PEAK_WIDTH, innerH, COLOR_WHITE);
        }

        gfx_->drawRect(barX - 1, barY - 1, VU_BAR_WIDTH + 2, VU_BAR_HEIGHT + 2, COLOR_DARK_GRAY);
    };

    auto updateBar = [&](Channel &ch, int &prevLen, int &prevPeak)
    {
        int target = clampi(ch.target, 0, VU_BAR_WIDTH);
        if (target > ch.avg)
        {
            int delta = target - ch.avg;
            int step = (delta > ATTACK_STEP) ? ATTACK_STEP : delta;
            ch.avg = ch.avg + step;
        }

        uint32_t now = millis();
        if (ch.avg - 1 > ch.peak)
        {
            ch.peak = ch.avg - 1;
            if (ch.peak < 0)
                ch.peak = -1;
            ch.peakExpiry = now + PEAK_HOLD_MS;
        }
        else if (ch.peak >= 0 && now >= ch.peakExpiry && ch.avg <= ch.peak)
        {
            ch.peak = -1;
        }

        drawVUBarDelta(ch, ch.avg, ch.peak, prevLen, prevPeak);
        prevLen = ch.avg;
        prevPeak = ch.peak;
    };

    auto decayIfDue = [&]()
    {
        uint32_t now = millis();
        if ((int32_t)(now - nextDecayAt) < 0)
            return;
        nextDecayAt = now + DECAY_INTERVAL_MS;
        if (chL.avg > 0)
            chL.avg = std::max(0, chL.avg - RELEASE_STEP);
        if (chR.avg > 0)
            chR.avg = std::max(0, chR.avg - RELEASE_STEP);
    };

    // ==================================================================================
    //                      MAIN PROCESSING LOOP
    // ==================================================================================

    static uint32_t last_frame_ms = 0;
    static int prevLenL = -1, prevLenR = -1, prevPeakL = -1, prevPeakR = -1;
    if (last_frame_ms == 0)
        last_frame_ms = millis();

    // Process one sample
    VUSample s;
    TickType_t wait_ticks = pdMS_TO_TICKS(10);
    const uint32_t FRAME_INTERVAL_MS = 20;

    if (xQueueReceive(queue_, &s, wait_ticks) == pdTRUE)
    {
        float l = std::isfinite(s.l_dbfs) ? s.l_dbfs : -120.0f;
        float r = std::isfinite(s.r_dbfs) ? s.r_dbfs : -120.0f;

        if (Config::VU_DISPLAY_ENABLED)
        {
            float ldb = l + Config::VU_DB_OFFSET;
            float rdb = r + Config::VU_DB_OFFSET;
            chL.target = dbToX(ldb);
            chR.target = dbToX(rdb);
        }
    }

    if (Config::VU_DISPLAY_ENABLED && gfx_)
    {
        uint32_t now_ms = millis();
        if ((int32_t)(now_ms - last_frame_ms) >= (int32_t)FRAME_INTERVAL_MS)
        {
            last_frame_ms = now_ms;
            // ---- RDS Status Bar (small text) ----
            if (Config::DISPLAY_SHOW_RDS_STATUS_BAR)
            {
                static uint32_t last_rds_ms = 0;
                static char last_line[64] = {0};
                if (now_ms - last_rds_ms >= 500)
                {
                    last_rds_ms = now_ms;
                    // Delay first status draw slightly to allow initial frame to settle
                    static uint32_t boot_ms0 = 0;
                    if (boot_ms0 == 0)
                        boot_ms0 = millis();
                    if ((now_ms - boot_ms0) < 1500)
                    {
                        strncpy(last_line, "", sizeof(last_line));
                        // Skip drawing until ready
                    }
                    else
                    {
                        // Build status string: PI, PTY short, flags
                        uint16_t pi = RDSAssembler::getPI();
                        uint8_t pty = RDSAssembler::getPTY();
                        bool tp = RDSAssembler::getTP();
                        bool ta = RDSAssembler::getTA();
                        bool ms = RDSAssembler::getMS();
                        bool st = DSP_pipeline::getStereoEnable();
                        bool rds = DSP_pipeline::getRdsEnable();
                        bool pil = DSP_pipeline::getPilotActive();

                        auto pty_name = [](uint8_t code) -> const char *
                        {
                            auto e = findPtyByCode(code);
                            return e ? e->short_label : "UNK";
                        };

                        char line[64];
                        // Include flag states so we detect changes and redraw
                        snprintf(line, sizeof(line),
                                 "PI=%04X PTY=%s ST=%u RDS=%u PIL=%u TP=%u TA=%u MS=%u",
                                 (unsigned)pi, pty_name(pty), st ? 1u : 0u, rds ? 1u : 0u,
                                 pil ? 1u : 0u, tp ? 1u : 0u, ta ? 1u : 0u, ms ? 1u : 0u);

                        if (strncmp(line, last_line, sizeof(last_line)) != 0)
                        {
                            // Clear bar region and print tokens with per-flag color
                            const int STATUS_H = 12; // roughly one text line
                            gfx_->fillRect(MARGIN_X, STATUS_Y - 2, VU_WIDTH, STATUS_H + 4,
                                           COLOR_BLACK);
                            gfx_->setTextSize(1);
                            gfx_->setTextWrap(false);

                            auto printToken = [&](const char *txt, uint16_t color)
                            {
                                gfx_->setTextColor(color);
                                gfx_->print(txt);
                            };

                            // Left: PI and PTY
                            gfx_->setCursor(MARGIN_X, STATUS_Y);
                            char buf[24];
                            snprintf(buf, sizeof(buf), "PI %04X  ", (unsigned)pi);
                            printToken(buf, Config::UI::COLOR_TEXT);
                            snprintf(buf, sizeof(buf), "PTY %s  ", pty_name(pty));
                            printToken(buf, Config::UI::COLOR_TEXT);

                            // Status chips: ST RDS PIL | TP TA MS
                            auto chip = [&](const char *lbl, bool on, uint16_t on_col)
                            {
                                // small dot
                                int16_t x = gfx_->getCursorX();
                                int16_t y = gfx_->getCursorY();
                                uint16_t col = on ? on_col : Config::UI::COLOR_MUTED;
                                gfx_->fillRect(x, y + 2, 6, 6, col);
                                gfx_->setCursor(x + 8, y);
                                printToken(lbl,
                                           on ? Config::UI::COLOR_TEXT : Config::UI::COLOR_DIM);
                                gfx_->setCursor(gfx_->getCursorX() + 6, y); // gap
                            };

                            chip("ST", st, Config::UI::COLOR_GOOD);
                            chip("RDS", rds, Config::UI::COLOR_GOOD);
                            chip("PIL", pil, Config::UI::COLOR_GOOD);
                            chip("TP", tp, Config::UI::COLOR_GOOD);
                            chip("TA", ta, ta ? Config::UI::COLOR_WARN : Config::UI::COLOR_GOOD);
                            chip("MS", ms, Config::UI::COLOR_GOOD);

                            strncpy(last_line, line, sizeof(last_line) - 1);
                            last_line[sizeof(last_line) - 1] = '\0';
                        }
                    }
                }
            }
            updateBar(chL, prevLenL, prevPeakL);
            updateBar(chR, prevLenR, prevPeakR);
            decayIfDue();
        }

        // Draw PS (centered) and RT (scrolling by characters) between title and VU meters
        {
            static uint32_t last_fetch_ms = 0;
            static char ps[9] = {0};
            static char rt_ui[1024] = {0};
            if (now_ms - last_fetch_ms >= 500)
            {
                last_fetch_ms = now_ms;
                RDSAssembler::getPS(ps);
                // Legacy path kept (not used for marquee building anymore)
                // s_ui_rt_long may be set by other modules, but marquee is derived from RTLIST.
                strncpy(rt_ui, s_ui_rt_long, sizeof(rt_ui) - 1);
                rt_ui[sizeof(rt_ui) - 1] = '\0';
            }

            // Layout region and font sizes
            const int TEXT_AREA_X = MARGIN_X;
            const int TEXT_AREA_W = VU_WIDTH;
            const int CHAR_W = 6;
            const int CHAR_H = 8;
            const int PS_SIZE = 3;
            const int RT_SIZE = 2;
            const int PS_H = CHAR_H * PS_SIZE;
            const int RT_H = CHAR_H * RT_SIZE;
            // TEXT_PS_Y is defined at file scope and already includes UI_SHIFT_Y
            const int TEXT_RT_Y = TEXT_PS_Y + PS_H + 6;

            // Draw PS centered (size 3); only if changed
            gfx_->setTextSize(PS_SIZE);

            // Create trimmed version: copy PS and remove trailing spaces
            char ps_trimmed[9];
            memset(ps_trimmed, 0, sizeof(ps_trimmed)); // Clear entire buffer
            strncpy(ps_trimmed, ps, 8);
            // Trim from the end: find last non-space character
            for (int i = 7; i >= 0; --i)
            {
                if (ps_trimmed[i] != ' ')
                {
                    ps_trimmed[i + 1] = '\0'; // Null-terminate after last non-space
                    break;
                }
                if (i == 0)
                {
                    ps_trimmed[0] = '\0'; // All spaces case
                }
            }

            // Calculate position based on trimmed length for centering
            int ps_len = (int)strlen(ps_trimmed);
            // Approximate pixel width: each character is 6 pixels at size 1, scales with text size
            int ps_px = ps_len * CHAR_W * PS_SIZE;
            int ps_x = TEXT_AREA_X + (TEXT_AREA_W - ps_px) / 2;
            if (ps_x < TEXT_AREA_X)
                ps_x = TEXT_AREA_X;

            static char ps_prev[9] = {0};

            if (strncmp(ps, ps_prev, 8) != 0)
            {
                // Clear entire PS area completely
                gfx_->fillRect(TEXT_AREA_X, TEXT_PS_Y - 2, TEXT_AREA_W, PS_H + 4,
                               Config::UI::COLOR_BG);

                // Draw trimmed PS centered with drop shadow and accent
                gfx_->setTextColor(Config::UI::COLOR_MUTED);
                gfx_->setCursor(ps_x + 1, TEXT_PS_Y + 1);
                gfx_->print(ps_trimmed);
                gfx_->setTextColor(Config::UI::COLOR_ACCENT);
                gfx_->setCursor(ps_x, TEXT_PS_Y);
                gfx_->print(ps_trimmed);

                // Update previous values
                memcpy(ps_prev, ps, 8);
                ps_prev[8] = '\0';
            }
            // PTY pill removed per request (avoid duplication next to PS)

            // Draw RT scrolling by characters (size 2)
            gfx_->setTextSize(RT_SIZE);
            static uint32_t last_scroll_ms = 0;
            // Long-form marquee buffers (current + pending), plus display buffer
            static char marquee_cur[1024] = {0};
            static char marquee_pending[1024] = {0};
            static bool has_pending = false;
            static char rt_disp[256] = {0};
            static int rt_off = 0;

            // Build marquee from RTLIST (concatenate items with a separator) and only
            // recompute when the set of RT items changes (ADD/DEL/CLEAR). If the list
            // is empty, fall back to the current broadcast RT (64 chars) so encoder
            // display roughly matches receivers.
            auto sanitize_ascii = [](const char *in, char *out, size_t out_sz)
            {
                size_t pos = 0;
                for (const char *p = in; *p && pos < out_sz - 1; ++p)
                {
                    unsigned char c = (unsigned char)*p;
                    if (c >= 0x20 && c < 0x7F)
                    {
                        out[pos++] = (char)c;
                    }
                    else if (c == '\t')
                    {
                        out[pos++] = ' ';
                    }
                    // drop other control/non-ASCII
                }
                out[pos] = '\0';
            };

            auto build_marquee_from_rtlist = [&](char *out, size_t out_sz)
            {
                out[0] = '\0';
                size_t out_pos = 0;
                const char *delim = " - "; // ASCII-safe delimiter for display
                const size_t delim_len = strlen(delim);

                std::size_t n = RDSAssembler::rtListCount();
                if (n == 0)
                {
                    // Fallback: use current broadcast RT window
                    static char rt64[65];
                    RDSAssembler::getRT(rt64);
                    // Trim trailing spaces
                    for (int i = 63; i >= 0; --i)
                    {
                        if (rt64[i] == ' ')
                            rt64[i] = '\0';
                        else
                            break;
                    }
                    static char clean[256];
                    sanitize_ascii(rt64, clean, sizeof(clean));
                    size_t len = strnlen(clean, sizeof(clean) - 1);
                    if (len > out_sz - 1)
                        len = out_sz - 1;
                    memcpy(out, clean, len);
                    out[len] = '\0';
                }
                else
                {
                    bool first = true;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        static char item[256];
                        if (!RDSAssembler::rtListGet(i, item, sizeof(item)))
                            continue;
                        static char clean[256];
                        sanitize_ascii(item, clean, sizeof(clean));
                        // Trim spaces at both ends
                        const char *s = clean;
                        while (*s == ' ')
                            ++s;
                        size_t sl = strlen(s);
                        while (sl > 0 && s[sl - 1] == ' ')
                        {
                            ((char *)s)[sl - 1] = '\0';
                            --sl;
                        }
                        if (sl == 0)
                            continue;
                        if (!first)
                        {
                            if (out_pos + delim_len < out_sz - 1)
                            {
                                memcpy(out + out_pos, delim, delim_len);
                                out_pos += delim_len;
                            }
                        }
                        first = false;
                        size_t to_copy = sl;
                        if (out_pos + to_copy >= out_sz - 1)
                            to_copy = (out_sz - 1) - out_pos;
                        memcpy(out + out_pos, s, to_copy);
                        out_pos += to_copy;
                        if (out_pos >= out_sz - 1)
                            break;
                    }
                    out[out_pos] = '\0';
                }

                // Add a seam between end and beginning so wrap reads naturally
                size_t cur_len = strlen(out);
                if (cur_len > 0)
                {
                    std::size_t n = RDSAssembler::rtListCount();
                    if (n > 1)
                    {
                        // Same spacing as between items
                        size_t need = delim_len;
                        if (cur_len + need > out_sz - 1)
                            need = (out_sz - 1) - cur_len;
                        memcpy(out + cur_len, delim, need);
                        out[cur_len + need] = '\0';
                    }
                    else
                    {
                        // Single RT fallback: add a few spaces so repeats aren't glued
                        const char gap[] = "      ";
                        size_t gap_len = sizeof(gap) - 1;
                        if (cur_len + gap_len > out_sz - 1)
                            gap_len = (out_sz - 1) - cur_len;
                        memcpy(out + cur_len, gap, gap_len);
                        out[cur_len + gap_len] = '\0';
                    }
                }
            };

            // Track changes in the RT list or fallback RT text using a signature string
            static char last_sig[1024] = {0};
            static char sig[1024];
            sig[0] = '\0';
            {
                std::size_t n = RDSAssembler::rtListCount();
                if (n == 0)
                {
                    char rt64[65];
                    RDSAssembler::getRT(rt64);
                    // Use the RT value as signature when list is empty (so we refresh on RT
                    // changes)
                    strncpy(sig, rt64, sizeof(sig) - 1);
                    sig[sizeof(sig) - 1] = '\0';
                }
                else
                {
                    bool first = true;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        char item[256];
                        if (RDSAssembler::rtListGet(i, item, sizeof(item)))
                        {
                            if (!first)
                                strncat(sig, "|", sizeof(sig) - strlen(sig) - 1);
                            strncat(sig, item, sizeof(sig) - strlen(sig) - 1);
                            first = false;
                        }
                    }
                }
            }

            if (strncmp(sig, last_sig, sizeof(last_sig) - 1) != 0)
            {
                strncpy(last_sig, sig, sizeof(last_sig) - 1);
                last_sig[sizeof(last_sig) - 1] = '\0';
                static char built[1024];
                built[0] = '\0';
                build_marquee_from_rtlist(built, sizeof(built));
                if (strncmp(built, marquee_cur, sizeof(marquee_cur)) != 0)
                {
                    strncpy(marquee_pending, built, sizeof(marquee_pending) - 1);
                    marquee_pending[sizeof(marquee_pending) - 1] = '\0';
                    has_pending = true;
                }
            }

            // Only update (clear + redraw) on scroll interval
            const uint32_t scroll_interval_ms = 200; // slower, less flicker
            if (now_ms - last_scroll_ms >= scroll_interval_ms)
            {
                last_scroll_ms = now_ms;

                // Commit pending marquee immediately if nothing to show
                if (marquee_cur[0] == '\0' && has_pending)
                {
                    strncpy(marquee_cur, marquee_pending, sizeof(marquee_cur) - 1);
                    marquee_cur[sizeof(marquee_cur) - 1] = '\0';
                    has_pending = false;
                    rt_off = 0;
                }

                // Compute display capacity in characters
                int cap_chars = TEXT_AREA_W / (CHAR_W * RT_SIZE);
                if (cap_chars < 1)
                    cap_chars = 1;
                if (cap_chars >= (int)sizeof(rt_disp))
                    cap_chars = (int)sizeof(rt_disp) - 1;

                // If fits, just draw once; else rotate offset by 1 char
                const size_t src_len = strnlen(marquee_cur, sizeof(marquee_cur) - 1);
                if (src_len > 0)
                {
                    // Build substring starting at rt_off
                    for (int i = 0; i < cap_chars; ++i)
                    {
                        rt_disp[i] = marquee_cur[(rt_off + i) % src_len];
                    }
                    rt_disp[cap_chars] = '\0';
                    rt_off = (rt_off + 1) % (int)src_len;

                    // On full wrap, swap in pending marquee if available
                    if (rt_off == 0 && has_pending)
                    {
                        strncpy(marquee_cur, marquee_pending, sizeof(marquee_cur) - 1);
                        marquee_cur[sizeof(marquee_cur) - 1] = '\0';
                        has_pending = false;
                        // rt_off already 0 at wrap; continue with new content next tick
                    }
                }
                else
                {
                    rt_disp[0] = '\0';
                }

                // Clear RT line and draw new (dim text, accent separators)
                gfx_->fillRect(TEXT_AREA_X, TEXT_RT_Y - 2, TEXT_AREA_W, RT_H + 4,
                               Config::UI::COLOR_BG);
                int cursor_x = TEXT_AREA_X;
                gfx_->setCursor(cursor_x, TEXT_RT_Y);
                for (int i = 0; rt_disp[i]; ++i)
                {
                    char c = rt_disp[i];
                    if (c == '-')
                        gfx_->setTextColor(Config::UI::COLOR_ACCENT);
                    else
                        gfx_->setTextColor(Config::UI::COLOR_DIM);
                    gfx_->print(c);
                }
            }
        }

        // Draw status panel
        VUStatsSnapshot stats;
        if (false && stats_queue_ && xQueueReceive(stats_queue_, &stats, 0) == pdTRUE)
        {
            const int panelY = DISPLAY_HEIGHT - Config::STATUS_PANEL_HEIGHT;
            gfx_->fillRect(0, panelY, DISPLAY_WIDTH, Config::STATUS_PANEL_HEIGHT, COLOR_BLACK);
            gfx_->setTextWrap(false);
            gfx_->setTextColor(COLOR_WHITE);
            gfx_->setTextSize(1);

            char line[64];
            int y = panelY + 2; // Shift up by 2 pixels (compress layout)

            if (stats.cpu_valid)
            {
                snprintf(line, sizeof(line), "Cores: Core0 %.1f%%  Core1 %.1f%%",
                         (double)stats.core0_load, (double)stats.core1_load);
            }
            else
            {
                snprintf(line, sizeof(line), "Cores: n/a (enable run-time stats)");
            }
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            snprintf(line, sizeof(line), "Audio proc: %.1f us (min %.1f, max %.1f)",
                     (double)stats.total_us_cur, (double)stats.total_us_min,
                     (double)stats.total_us_max);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            if (stats.cpu_valid)
            {
                snprintf(line, sizeof(line), "Tasks: Aud %.1f%%  Console %.1f%%  VU %.1f%%",
                         (double)stats.audio_cpu, (double)stats.logger_cpu, (double)stats.vu_cpu);
                gfx_->setCursor(4, y);
                gfx_->print(line);
                y += 12;
            }

            snprintf(line, sizeof(line), "Stages: FIR %.1f us  MPX %.1f us",
                     (double)stats.fir_us_cur, (double)stats.mpx_us_cur);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            snprintf(line, sizeof(line), "Stages: Mat %.1f us  RDS %.1f us",
                     (double)stats.matrix_us_cur, (double)stats.rds_us_cur);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            snprintf(line, sizeof(line), "Heap: %u KB (min %u)  Uptime: %u s",
                     (unsigned)(stats.heap_free / 1024u), (unsigned)(stats.heap_min / 1024u),
                     (unsigned)stats.uptime_s);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            snprintf(line, sizeof(line), "Rates: %u kHz -> %u kHz  Up %ux  Block %u  %u-bit",
                     (unsigned)(Config::SAMPLE_RATE_ADC / 1000u),
                     (unsigned)(Config::SAMPLE_RATE_DAC / 1000u), (unsigned)Config::UPSAMPLE_FACTOR,
                     (unsigned)Config::BLOCK_SIZE, (unsigned)Config::BITS_PER_SAMPLE);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            unsigned total_overflow = sample_overflow_count_ + stats_overflow_count_;
            snprintf(line, sizeof(line), "Loops: %u  Errors: %u  Overflow: %u",
                     (unsigned)stats.loops_completed, (unsigned)stats.errors, total_overflow);
            gfx_->setCursor(4, y);
            gfx_->print(line);
            y += 12;

            // Display compile date and time at the bottom
            snprintf(line, sizeof(line), "Compiled: %s %s", __DATE__, __TIME__);
            gfx_->setCursor(4, y);
            gfx_->print(line);
        }
    }
}

void DisplayManager::shutdown()
{
    if (queue_)
    {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (stats_queue_)
    {
        vQueueDelete(stats_queue_);
        stats_queue_ = nullptr;
    }
    if (gfx_)
    {
        delete gfx_;
        gfx_ = nullptr;
    }
    if (bus_)
    {
        delete bus_;
        bus_ = nullptr;
    }
}

// ==================================================================================
//                          INSTANCE MESSAGE ENQUEUE METHODS
// ==================================================================================

bool DisplayManager::enqueueRaw(const VUSample &s)
{
    if (!queue_)
    {
        ErrorHandler::logError(ErrorCode::QUEUE_NOT_INITIALIZED, "DisplayManager::enqueueRaw",
                               "queue is null");
        return false;
    }

    if (uxQueueSpacesAvailable(queue_) == 0 && uxQueueMessagesWaiting(queue_) == 1)
    {
        xQueueOverwrite(queue_, (void *)&s);
        sample_overflow_count_++;
        // Log first overflow only to prevent log spam
        if (!sample_overflow_logged_)
        {
            sample_overflow_logged_ = true;
            ErrorHandler::logWarning("DisplayManager::enqueueRaw",
                                     "sample queue overflow (overwrite mode)");
        }
        return true;
    }

    if (xQueueSend(queue_, &s, 0) != pdTRUE)
    {
        sample_overflow_count_++;
        // Log first overflow only to prevent log spam
        if (!sample_overflow_logged_)
        {
            sample_overflow_logged_ = true;
            ErrorHandler::logError(ErrorCode::QUEUE_FULL, "DisplayManager::enqueueRaw",
                                   "sample queue full, sample dropped");
        }
        return false;
    }

    return true;
}

bool DisplayManager::enqueueFromISRRaw(const VUSample &s, BaseType_t *pxHigherPriorityTaskWoken)
{
    if (!queue_)
    {
        // Cannot log from ISR context - just track the error
        sample_overflow_count_++;
        return false;
    }

    BaseType_t x = xQueueSendFromISR(queue_, (void *)&s, pxHigherPriorityTaskWoken);
    if (x != pdTRUE)
    {
        sample_overflow_count_++;
#if (INCLUDE_xQueueOverwriteFromISR == 1)
        x = xQueueOverwriteFromISR(queue_, (void *)&s, pxHigherPriorityTaskWoken);
        if (x == pdTRUE)
            return true;
#endif
        return false;
    }
    return true;
}

bool DisplayManager::enqueueStatsRaw(const VUStatsSnapshot &s)
{
    if (!stats_queue_)
    {
        ErrorHandler::logError(ErrorCode::QUEUE_NOT_INITIALIZED, "DisplayManager::enqueueStatsRaw",
                               "stats queue is null");
        return false;
    }

    if (uxQueueSpacesAvailable(stats_queue_) == 0 && uxQueueMessagesWaiting(stats_queue_) == 1)
    {
        xQueueOverwrite(stats_queue_, (void *)&s);
        stats_overflow_count_++;
        return true;
    }

    if (xQueueSend(stats_queue_, &s, 0) != pdTRUE)
    {
        stats_overflow_count_++;
        // Log stats overflow less frequently (not every time to prevent spam)
        if (stats_overflow_count_ == 1 || (stats_overflow_count_ % 100 == 0))
        {
            ErrorHandler::logWarning("DisplayManager::enqueueStatsRaw",
                                     "stats queue full, snapshot dropped");
        }
        return false;
    }

    return true;
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
