/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                       RDS Assembler (TaskBaseClass)
 *
 * =====================================================================================
 *
 * File:         RDSAssembler.h
 * Description:  RDS Bitstream Generator Module
 *
 * Purpose
 *   Produces the RDS bitstream (1187.5 bps) on a non-real-time core. The audio core
 *   reads bits via a non-blocking API and synthesizes the 57 kHz RDS injection
 *   synchronized to the DAC sample clock (Config::SAMPLE_RATE_DAC).
 *
 * Design
 *   - Runs as a FreeRTOS task pinned to Core 1 (logger/display core)
 *   - Writes bits into a single-producer/single-consumer FreeRTOS queue
 *   - Overwrite semantics: if queue is full, oldest bit is dropped (freshness wins)
 *   - Implements core RDS groups with proper CRC-10 and offset words:
 *       • Group 0A: PI (A), TP/PTY/TA/MS (B), AF codes (C), PS name (D)
 *       • Group 2A: PI (A), TP/PTY + RT A/B + segment (B), RT text (C/D)
 *     Cycles groups in a simple scheduler (0A, 0A, 2A, …) so PS/flags are
 *     transmitted twice as often as RT. RT A/B flag toggles on setRT() to
 *     force receiver refresh. No CT (4A) or advanced groups are emitted.
 *
 * Threading model
 *   - Producer: Assembler task (Core 1)
 *   - Consumer: DSP_pipeline (Core 0) calls nextBit() from the DAC-rate path
 *
 */
#pragma once

#include "ErrorHandler.h"
#include "TaskBaseClass.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

/**
 * RDSAssembler - RDS Bitstream Generator
 *
 * Inherits from TaskBaseClass to provide unified FreeRTOS task lifecycle management.
 * Generates RDS bitstream (1187.5 bps) on Core 1, allowing audio core to read bits
 * via non-blocking nextBit() API.
 *
 * Queue Semantics (See QueueContracts.md for full design rationale):
 *
 *   Bit Queue:
 *     * Type: FIFO with drop-oldest-on-overflow
 *     * Size: 1024 bits (128 bytes)
 *     * Element: 1 byte per bit (uint8_t: 0 or 1)
 *     * Behavior: Non-blocking, drop oldest bit when full
 *       - Buffer provides ~860 ms at 1187.5 bps
 *       - When full: xQueueReceive() oldest bit, then xQueueSend() new bit
 *       - Ensures RDS bitstream sequence always maintained
 *     * Rationale: RDS bits must be sequential. Large buffer (1024 bits)
 *       provides timing slack for asynchronous producer/consumer. If buffer
 *       fills, oldest bits are least critical (already transmitted). Dropping
 *       oldest bits allows system to recover synchronization by advancing.
 *     * Tradeoff: Cannot guarantee all bits transmitted, but maintains RDS
 *       sequence integrity and never blocks real-time audio code.
 *
 * Design Pattern:
 *   * Producer (RDSAssembler on Core 1): Generates bits at ~1187.5 bps
 *     - Builds RDS groups (26-bit blocks)
 *     - Converts to individual bits and enqueues
 *     - Non-blocking operation
 *   * Consumer (DSP_pipeline on Core 0): Reads bits at 57 kHz
 *     - nextBit() returns individual bits non-blockingly
 *     - Returns false if queue empty (normal condition)
 *     - Modulates bits onto 57 kHz carrier
 *
 * Thread Safety:
 *   All public functions are safe to call from any task.
 *   FreeRTOS queue provides atomic operations.
 *   Producer (RDSAssembler on Core 1) and Consumer (DSP_pipeline on Core 0)
 *   never block each other.
 *   nextBit() is non-blocking and always returns immediately.
 *
 * Queue Overflow Behavior (Bit Queue):
 *   Uses drop-oldest-on-overflow semantics with 1024-bit buffer.
 *   • Buffer holds ~860 ms of RDS bits at 1187.5 bps
 *   • When buffer full: oldest bits are dropped first
 *   • Maintains RDS sequence integrity through continuous bit stream
 *   • Prevents real-time audio code (Consumer) from being blocked
 *   • Allows system to recover from producer/consumer timing mismatches
 *   • Overflow counter tracks dropped bits for diagnostics
 *
 * Backward Compatibility:
 *   Static wrapper methods maintain compatibility with original namespace-based API.
 */
class RDSAssembler : public TaskBaseClass
{
  public:
    /**
     * Get RDSAssembler Singleton Instance
     *
     * Returns reference to the single global RDSAssembler instance.
     *
     * Returns:
     *   Reference to the singleton RDSAssembler instance
     */
    static RDSAssembler &getInstance();

    /**
     * Start the assembler task pinned to core_id, with the given priority and stack.
     * bit_queue_len controls the depth of the bit FIFO; 1024 is ample for safety.
     */
    static bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words,
                          size_t bit_queue_len = 1024);
    /** Stop RDS Assembler task (deletes the FreeRTOS task). */
    static void stopTask();
    /** Returns true once the assembler task is initialized and running. */
    static bool isReady();

    /**
     * Non-blocking fetch of the next bit. Returns true if a bit was available.
     */
    static bool nextBit(uint8_t &bit);

    // ===================== Builder Control APIs (to be implemented) =====================
    /**
     * Set Program Identification code (PI)
     */
    static void setPI(uint16_t pi);

    /**
     * Set Program Type (PTY, 0..31)
     */
    static void setPTY(uint8_t pty);

    /**
     * Set Traffic Program (TP) and Traffic Announcement (TA) flags
     */
    static void setTP(bool tp);
    static void setTA(bool ta);

    /**
     * Set Music/Speech flag (true = Music, false = Speech)
     */
    static void setMS(bool music);

    /**
     * Set Program Service name (exactly 8 chars, padded with spaces if shorter)
     */
    static void setPS(const char *ps);

    /**
     * Set Radio Text (up to 64 chars, padded or truncated by the builder)
     */
    static void setRT(const char *rt);

    /**
     * Get current Program Service (PS) 8-character name.
     * Copies 8 chars and appends a null terminator into out[9].
     */
    static void getPS(char out[9]);

    /**
     * Get current RadioText (RT) string (up to 64 chars).
     * Copies and null-terminates into out[65].
     */
    static void getRT(char out[65]);

    // Runtime getters for status queries
    static uint16_t getPI();
    static uint8_t getPTY();
    static bool getTP();
    static bool getTA();
    static bool getMS();
    static bool getRTAB();

    // RadioText rotation list API
    static void rtListAdd(const char *text);
    static bool rtListDel(std::size_t idx);
    static void rtListClear();
    static std::size_t rtListCount();
    static bool rtListGet(std::size_t idx, char *out, std::size_t out_sz);
    static void setRtPeriod(uint32_t seconds);
    static uint32_t getRtPeriod();

    /**
     * Set AF list (FM VHF, Method A). freqs_mhz: array of MHz (e.g., 101.1f).
     * Only 87.6-107.9 MHz are encoded (0.1 MHz step). Max 25 entries per spec.
     */
    static void setAF_FM(const float *freqs_mhz, size_t count);

    /**
     * Set Clock-Time (Group 4A). All parameters are local time (not UTC).
     * offset_half_hours: local time offset from UTC in 30-minute steps (e.g., +2h = +4)
     */
    static void setClock(int year, int month, int day, int hour, int minute,
                         int8_t offset_half_hours);

  private:
    /**
     * Private Constructor (Singleton Pattern)
     *
     * Initializes module state. Only called by getInstance().
     */
    RDSAssembler();

    /**
     * Virtual Destructor Implementation
     */
    virtual ~RDSAssembler() = default;

    /**
     * Initialize Module Resources (TaskBaseClass contract)
     *
     * Called once when the task starts. Creates the bit queue.
     *
     * Returns:
     *   true if initialization successful, false otherwise
     */
    bool begin() override;

    /**
     * Main Processing Loop Body (TaskBaseClass contract)
     *
     * Called repeatedly in infinite loop. Generates RDS bits and enqueues them.
     */
    void process() override;

    /**
     * Shutdown Module Resources (TaskBaseClass contract)
     *
     * Called during graceful shutdown. Cleans up queue resources.
     */
    void shutdown() override;

    /**
     * Task Trampoline (FreeRTOS Entry Point)
     *
     * Static function called by FreeRTOS when task starts.
     * Delegates to TaskBaseClass::defaultTaskTrampoline().
     *
     * Parameters:
     *   arg: Pointer to RDSAssembler instance
     */
    static void taskTrampoline(void *arg);

    /**
     * Instance Method - Fetch next bit
     *
     * Core implementation of static nextBit().
     */
    bool nextBitRaw(uint8_t &bit);

    // Member State Variables
    QueueHandle_t bit_queue_; ///< FreeRTOS queue for RDS bits
    size_t bit_queue_len_;    ///< Queue depth in bits
    int core_id_;             ///< FreeRTOS core ID
    UBaseType_t priority_;    ///< Task priority
    uint32_t stack_words_;    ///< Stack size in words

    // RDS builder state (member variables)
    uint16_t pi_;          ///< Program Identification
    uint8_t pty_;          ///< Program Type (0..31)
    bool tp_;              ///< Traffic Program flag
    bool ta_;              ///< Traffic Announcement flag
    bool ms_;              ///< Music (true) / Speech (false)
    char ps_[8];           ///< Program Service name
    char rt_[64];          ///< Radio Text
    bool rt_ab_;           ///< Text A/B flag
    uint8_t af_codes_[25]; ///< AF codes
    uint8_t af_count_;     ///< Number of valid AF codes
    uint8_t af_cursor_;    ///< Rotating index into AF codes
    bool ct_enabled_;      ///< CT enabled flag
    uint16_t ct_mjd_;      ///< Modified Julian Date
    uint8_t ct_hour_;      ///< Local hour 0..23
    uint8_t ct_min_;       ///< Local minute 0..59
    bool ct_lto_neg_;      ///< Local time offset sign
    uint8_t ct_lto_hh_;    ///< Local time offset magnitude

    // Rotation list state (was file-static globals)
    std::vector<std::string> rt_list_;
    uint32_t rt_period_s_ = 30;
    std::size_t rt_index_ = 0;
    uint64_t rt_next_switch_us_ = 0;

    // Error Tracking
    volatile uint32_t bit_overflow_count_; ///< Count of bit queue overflows
    volatile bool bit_overflow_logged_;    ///< First overflow logged flag (prevent spam)
};

// Legacy namespace-based wrapper removed

// =====================================================================================
//                                END OF FILE
// =====================================================================================
