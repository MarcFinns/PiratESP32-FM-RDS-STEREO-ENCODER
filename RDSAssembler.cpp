/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       RDS Assembler (ModuleBase Implementation)
 *
 * =====================================================================================
 *
 * File:         RDSAssembler.cpp
 * Description:  RDS Bitstream Generator Module Implementation
 *
 * Purpose:
 *   Generates the RDS bitstream (1187.5 bps) on a non-real-time core (Core 1).
 *   The audio core (Core 0) reads bits via a non-blocking API and synthesizes the
 *   57 kHz RDS injection synchronized to the 192 kHz sample clock.
 *
 * =====================================================================================
 */
#include "RDSAssembler.h"
#include "DisplayManager.h"

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "Console.h"
#include <esp_timer.h>

#include <freertos/queue.h>
#include <freertos/task.h>

// ==================================================================================
//                          SINGLETON INSTANCE
// ==================================================================================

RDSAssembler &RDSAssembler::getInstance()
{
    static RDSAssembler s_instance;
    return s_instance;
}

// ==================================================================================
//                          CONSTRUCTOR & MEMBER INITIALIZATION
// ==================================================================================

RDSAssembler::RDSAssembler()
    : bit_queue_(nullptr), bit_queue_len_(1024), core_id_(1), priority_(1), stack_words_(4096),
      pi_(0x1234), pty_(0), tp_(false), ta_(false), ms_(true), rt_ab_(false), af_count_(0),
      af_cursor_(0), ct_enabled_(false), ct_mjd_(0), ct_hour_(0), ct_min_(0), ct_lto_neg_(false),
      ct_lto_hh_(0), bit_overflow_count_(0), bit_overflow_logged_(false)
{
    // Initialize PS (Program Service name)
    ps_[0] = 'E';
    ps_[1] = 'S';
    ps_[2] = 'P';
    ps_[3] = '3';
    ps_[4] = '2';
    ps_[5] = ' ';
    ps_[6] = 'F';
    ps_[7] = 'M';

    // Initialize RT (Radio Text)
    const char default_rt[] = "Hello RDS on ESP32!                                              ";
    for (int i = 0; i < 64; ++i)
        rt_[i] = default_rt[i];

    // Initialize AF codes
    for (int i = 0; i < 25; ++i)
        af_codes_[i] = 0;
}

// (Rotation state moved to class members)

// ==================================================================================
//                          STATIC WRAPPER API
// ==================================================================================

bool RDSAssembler::startTask(int core_id, uint32_t priority, uint32_t stack_words,
                             size_t bit_queue_len)
{
    RDSAssembler &rds = getInstance();
    rds.bit_queue_len_ = bit_queue_len;
    if (rds.bit_queue_len_ == 0)
        rds.bit_queue_len_ = 1024;
    rds.core_id_ = core_id;
    rds.priority_ = priority;
    rds.stack_words_ = stack_words;

    // Spawn assembler task via ModuleBase helper
    return rds.spawnTask("rds_asm",
                         (uint32_t)stack_words,
                         (UBaseType_t)priority,
                         core_id,
                         RDSAssembler::taskTrampoline);
}

bool RDSAssembler::nextBit(uint8_t &bit)
{
    return getInstance().nextBitRaw(bit);
}

void RDSAssembler::setPI(uint16_t pi)
{
    getInstance().pi_ = pi;
}
void RDSAssembler::setPTY(uint8_t pty)
{
    getInstance().pty_ = (uint8_t)(pty & 0x1Fu);
}
void RDSAssembler::setTP(bool tp)
{
    getInstance().tp_ = tp;
}
void RDSAssembler::setTA(bool ta)
{
    getInstance().ta_ = ta;
}
void RDSAssembler::setMS(bool music)
{
    getInstance().ms_ = music;
}

void RDSAssembler::setPS(const char *ps)
{
    RDSAssembler &rds = getInstance();
    if (!ps)
        return;
    // Copy up to 8 characters, then pad with spaces.
    // Avoid reading beyond the provided string to prevent garbage copy.
    size_t len = 0;
    while (len < 8 && ps[len] != '\0')
        ++len;
    for (size_t i = 0; i < len; ++i)
        rds.ps_[i] = ps[i];
    for (size_t i = len; i < 8; ++i)
        rds.ps_[i] = ' ';
}

void RDSAssembler::setRT(const char *rt)
{
    RDSAssembler &rds = getInstance();
    if (!rt)
        return;
    // Only update the 64-character broadcast window (A/B toggle). Display marquee
    // is managed independently by DisplayManager from RTLIST or fallback RT.
    int i = 0;
    for (; i < 64 && rt[i] != '\0'; ++i)
        rds.rt_[i] = rt[i];
    for (; i < 64; ++i)
        rds.rt_[i] = ' ';
    rds.rt_ab_ = !rds.rt_ab_;
}

void RDSAssembler::getPS(char out[9])
{
    RDSAssembler &rds = getInstance();
    for (int i = 0; i < 8; ++i)
        out[i] = rds.ps_[i];
    out[8] = '\0';
}

void RDSAssembler::getRT(char out[65])
{
    RDSAssembler &rds = getInstance();
    int i = 0;
    for (; i < 64; ++i)
        out[i] = rds.rt_[i];
    out[64] = '\0';
}

void RDSAssembler::setAF_FM(const float *freqs_mhz, size_t count)
{
    RDSAssembler &rds = getInstance();
    rds.af_count_ = 0;
    rds.af_cursor_ = 0;
    if (!freqs_mhz || count == 0)
        return;

    for (size_t i = 0; i < count && rds.af_count_ < 25; ++i)
    {
        // Map frequency to AF code (1-204)
        int tenths = (int)(freqs_mhz[i] * 10.0f + 0.5f);
        int n = tenths - 875;
        if (n < 1 || n > 204)
            continue;

        uint8_t code = (uint8_t)n;
        bool exists = false;
        for (uint8_t k = 0; k < rds.af_count_; ++k)
        {
            if (rds.af_codes_[k] == code)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
            rds.af_codes_[rds.af_count_++] = code;
    }
}

void RDSAssembler::setClock(int year, int month, int day, int hour, int minute,
                            int8_t offset_half_hours)
{
    RDSAssembler &rds = getInstance();
    if (hour < 0)
        hour = 0;
    if (hour > 23)
        hour = 23;
    if (minute < 0)
        minute = 0;
    if (minute > 59)
        minute = 59;

    rds.ct_hour_ = (uint8_t)hour;
    rds.ct_min_ = (uint8_t)minute;
    rds.ct_lto_neg_ = (offset_half_hours < 0);
    int off = offset_half_hours < 0 ? -offset_half_hours : offset_half_hours;
    if (off > 31)
        off = 31;
    rds.ct_lto_hh_ = (uint8_t)off;

    // Compute MJD from date (Fliegel-Van Flandern algorithm)
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    long jdn = day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
    long mjd = jdn - 2400001;
    if (mjd < 0)
        mjd = 0;
    if (mjd > 65535)
        mjd = 65535;
    rds.ct_mjd_ = (uint16_t)mjd;
    rds.ct_enabled_ = true;
}

// ==================================================================================
//                          MODULEBASE IMPLEMENTATION
// ==================================================================================

void RDSAssembler::taskTrampoline(void *arg)
{
    ModuleBase::defaultTaskTrampoline(arg);
}

bool RDSAssembler::begin()
{
    Console::enqueuef(LogLevel::INFO, "RDSAssembler running on Core %d", xPortGetCoreID());
    // Create FreeRTOS queue for RDS bits
    bit_queue_ = xQueueCreate((UBaseType_t)bit_queue_len_, sizeof(uint8_t));
    if (!bit_queue_)
    {
        ErrorHandler::logError(ErrorCode::INIT_QUEUE_FAILED, "RDSAssembler::begin",
                               "bit queue creation failed");
        return false;
    }

    // Initialize default CT if not already set
    if (!ct_enabled_)
        setClock(1985, 10, 26, 0, 0, 0);

    ErrorHandler::logInfo("RDSAssembler", "Task initialized successfully");
    return true;
}

void RDSAssembler::process()
{
    // ==================================================================================
    //                      RDS HELPER FUNCTIONS
    // ==================================================================================

    auto crc10 = [](uint16_t info) -> uint16_t
    {
        uint32_t reg = (uint32_t)info << 10;
        const uint32_t poly = 0x5B9u;
        for (int i = 25; i >= 10; --i)
        {
            if (reg & (1u << i))
                reg ^= (poly << (i - 10));
        }
        return (uint16_t)(reg & 0x3FFu);
    };

    constexpr uint16_t kOffsetA = 0x0FC;
    constexpr uint16_t kOffsetB = 0x198;
    constexpr uint16_t kOffsetC = 0x168;
    constexpr uint16_t kOffsetD = 0x1B4;
    constexpr uint16_t kOffsetCp = 0x1CC;

    auto enqueueBlock = [&](uint16_t info, uint16_t offset)
    {
        uint16_t cw = crc10(info) ^ (offset & 0x3FFu);
        uint32_t block26 = ((uint32_t)info << 10) | cw;
        for (int i = 25; i >= 0; --i)
        {
            uint8_t bit = (block26 >> i) & 1u;
            if (bit_queue_)
            {
                if (uxQueueSpacesAvailable(bit_queue_) == 0 &&
                    uxQueueMessagesWaiting(bit_queue_) >= 1)
                {
                    uint8_t dummy;
                    xQueueReceive(bit_queue_, &dummy, 0);
                }
                xQueueSend(bit_queue_, &bit, 0);
            }
        }
    };

    // ==================================================================================
    //                      GROUP BUILDING FUNCTIONS
    // ==================================================================================

    auto buildGroup2A = [&](uint8_t seg)
    {
        enqueueBlock(pi_, kOffsetA);

        uint16_t b = 0;
        b |= (2u << 12);
        b |= (tp_ ? 1u : 0u) << 10;
        b |= ((uint16_t)(pty_ & 0x1Fu)) << 5;
        b |= (rt_ab_ ? 1u : 0u) << 4;
        b |= (seg & 0x0Fu);
        enqueueBlock(b, kOffsetB);

        uint8_t i0 = seg * 4;
        uint16_t c = ((uint16_t)(uint8_t)rt_[i0 + 0] << 8) | (uint16_t)(uint8_t)rt_[i0 + 1];
        uint16_t d = ((uint16_t)(uint8_t)rt_[i0 + 2] << 8) | (uint16_t)(uint8_t)rt_[i0 + 3];
        enqueueBlock(c, kOffsetC);
        enqueueBlock(d, kOffsetD);
    };

    auto buildGroup0A = [&](uint8_t seg)
    {
        enqueueBlock(pi_, kOffsetA);

        uint16_t b = 0;
        b |= (tp_ ? 1u : 0u) << 10;
        b |= ((uint16_t)(pty_ & 0x1Fu)) << 5;
        b |= (ta_ ? 1u : 0u) << 4;
        b |= (ms_ ? 1u : 0u) << 3;
        b |= ((uint16_t)0u) << 2;
        b |= (seg & 0x03u);
        enqueueBlock(b, kOffsetB);

        uint8_t af1 = 0, af2 = 0;
        if (af_count_ > 0)
        {
            if (af_cursor_ == 0)
            {
                af1 = (uint8_t)(224 + af_count_);
                af2 = af_codes_[0];
                af_cursor_ = 1;
            }
            else
            {
                af1 = af_codes_[af_cursor_ % af_count_];
                af2 = af_codes_[(af_cursor_ + 1) % af_count_];
                af_cursor_ = (uint8_t)((af_cursor_ + 2) % af_count_);
            }
        }
        uint16_t c = ((uint16_t)af1 << 8) | af2;
        enqueueBlock(c, kOffsetC);

        uint8_t i0 = seg * 2;
        uint16_t d = ((uint16_t)(uint8_t)ps_[i0 + 0] << 8) | (uint16_t)(uint8_t)ps_[i0 + 1];
        enqueueBlock(d, kOffsetD);
    };

    // ==================================================================================
    //                      MAIN PROCESSING LOOP
    // ==================================================================================

    static uint32_t accu_us = 0;
    static uint8_t seg_ps = 0;
    static uint8_t seg_rt = 0;
    static uint8_t rot = 0;
    static bool first_time = true;

    if (first_time)
    {
        first_time = false;
        accu_us = 0;
    }

    const uint32_t bit_us = 842;
    vTaskDelay(pdMS_TO_TICKS(1));
    accu_us += 1000;

    // Handle RT rotation (pilot task cadence, coarse timer ok)
    if (!rt_list_.empty())
    {
        uint64_t now = esp_timer_get_time();
        if (rt_period_s_ > 0 && now >= rt_next_switch_us_)
        {
            if (rt_index_ >= rt_list_.size())
                rt_index_ = 0;
            const std::string &cur = rt_list_[rt_index_];
            setRT(cur.c_str());
            rt_index_ = (rt_index_ + 1) % rt_list_.size();
            rt_next_switch_us_ = now + (uint64_t)rt_period_s_ * 1000000ULL;
        }
    }

    while (accu_us >= bit_us)
    {
        accu_us -= bit_us;

        if (uxQueueMessagesWaiting(bit_queue_) < 26)
        {
            if (rot == 2)
            {
                buildGroup2A(seg_rt);
                seg_rt = (uint8_t)((seg_rt + 1) & 0x0F);
            }
            else
            {
                buildGroup0A(seg_ps);
                seg_ps = (uint8_t)((seg_ps + 1) & 0x03);
            }
            rot = (uint8_t)((rot + 1) % 3);
        }
    }
}

void RDSAssembler::shutdown()
{
    if (bit_queue_)
    {
        vQueueDelete(bit_queue_);
        bit_queue_ = nullptr;
    }
}

// ==================================================================================
//                          INSTANCE MESSAGE METHODS
// ==================================================================================

bool RDSAssembler::nextBitRaw(uint8_t &bit)
{
    if (!bit_queue_)
    {
        ErrorHandler::logError(ErrorCode::QUEUE_NOT_INITIALIZED, "RDSAssembler::nextBitRaw",
                               "bit queue is null");
        return false;
    }

    if (xQueueReceive(bit_queue_, &bit, 0) != pdTRUE)
    {
        // Queue empty - this is normal, not necessarily an error
        return false;
    }

    return true;
}

// ------------------- Runtime getters -------------------

uint16_t RDSAssembler::getPI()
{
    return getInstance().pi_;
}
uint8_t RDSAssembler::getPTY()
{
    return getInstance().pty_;
}
bool RDSAssembler::getTP()
{
    return getInstance().tp_;
}
bool RDSAssembler::getTA()
{
    return getInstance().ta_;
}
bool RDSAssembler::getMS()
{
    return getInstance().ms_;
}
bool RDSAssembler::getRTAB()
{
    return getInstance().rt_ab_;
}

// ------------------- Rotation list API -------------------

void RDSAssembler::rtListAdd(const char *text)
{
    if (!text)
        return;
    getInstance().rt_list_.emplace_back(text);
    if (getInstance().rt_list_.size() == 1)
    {
        // schedule immediate switch to this first item
        getInstance().rt_index_ = 0;
        getInstance().rt_next_switch_us_ = esp_timer_get_time();
    }
}

bool RDSAssembler::rtListDel(std::size_t idx)
{
    RDSAssembler &r = getInstance();
    if (idx >= r.rt_list_.size())
        return false;
    r.rt_list_.erase(r.rt_list_.begin() + idx);
    if (r.rt_index_ >= r.rt_list_.size())
        r.rt_index_ = 0;
    return true;
}

void RDSAssembler::rtListClear()
{
    RDSAssembler &r = getInstance();
    r.rt_list_.clear();
    r.rt_index_ = 0;
    r.rt_next_switch_us_ = 0;
}

std::size_t RDSAssembler::rtListCount()
{
    return getInstance().rt_list_.size();
}

bool RDSAssembler::rtListGet(std::size_t idx, char *out, std::size_t out_sz)
{
    RDSAssembler &r = getInstance();
    if (!out || out_sz == 0 || idx >= r.rt_list_.size())
        return false;
    const auto &s = r.rt_list_[idx];
    std::size_t n = std::min(out_sz - 1, s.size());
    memcpy(out, s.data(), n);
    out[n] = '\0';
    return true;
}

void RDSAssembler::setRtPeriod(uint32_t seconds)
{
    RDSAssembler &r = getInstance();
    r.rt_period_s_ = seconds;
    r.rt_next_switch_us_ = esp_timer_get_time();
}

uint32_t RDSAssembler::getRtPeriod()
{
    return getInstance().rt_period_s_;
}

// =====================================================================================
//                                END OF FILE
// =====================================================================================
