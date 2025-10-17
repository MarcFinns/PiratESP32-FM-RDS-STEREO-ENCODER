/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                       RDS Assembler (ModuleBase)
 *
 * =====================================================================================
 *
 * File:         RDSAssembler.h
 * Description:  RDS Bitstream Generator Module
 *
 * Purpose
 *   Produces the RDS bitstream (1187.5 bps) on a non-real-time core. The audio core
 *   reads bits via a non-blocking API and synthesizes the 57 kHz RDS injection
 *   synchronized to the 192 kHz sample clock.
 *
 * Design
 *   - Runs as a FreeRTOS task pinned to Core 1 (logger/display core)
 *   - Writes bits into a single-producer/single-consumer FreeRTOS queue
 *   - Overwrite semantics: if queue is full, oldest bit is dropped (freshness wins)
 *   - Skeleton content: currently outputs a PRBS placeholder; replace with real RDS
 *     group scheduler, CRC/checkwords, and offset words later
 *
 * Threading model
 *   - Producer: Assembler task (Core 1)
 *   - Consumer: DSP_pipeline (Core 0) calls nextBit() from the 192 kHz path
 *
 */
#pragma once

#include "ErrorHandler.h"
#include "ModuleBase.h"

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

/**
 * RDSAssembler - RDS Bitstream Generator
 *
 * Inherits from ModuleBase to provide unified FreeRTOS task lifecycle management.
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
class RDSAssembler : public ModuleBase
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
    static bool startTask(int core_id, uint32_t priority, uint32_t stack_words,
                          size_t bit_queue_len = 1024);

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
     * Initialize Module Resources (ModuleBase contract)
     *
     * Called once when the task starts. Creates the bit queue.
     *
     * Returns:
     *   true if initialization successful, false otherwise
     */
    bool begin() override;

    /**
     * Main Processing Loop Body (ModuleBase contract)
     *
     * Called repeatedly in infinite loop. Generates RDS bits and enqueues them.
     */
    void process() override;

    /**
     * Shutdown Module Resources (ModuleBase contract)
     *
     * Called during graceful shutdown. Cleans up queue resources.
     */
    void shutdown() override;

    /**
     * Task Trampoline (FreeRTOS Entry Point)
     *
     * Static function called by FreeRTOS when task starts.
     * Delegates to ModuleBase::defaultTaskTrampoline().
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
    uint32_t priority_;       ///< Task priority
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

    // Error Tracking
    volatile uint32_t bit_overflow_count_; ///< Count of bit queue overflows
    volatile bool bit_overflow_logged_;    ///< First overflow logged flag (prevent spam)
};

// Backward compatibility namespace wrapper
namespace RDSAssembler_NS
{
inline bool startTask(int core_id, uint32_t priority, uint32_t stack_words,
                      size_t bit_queue_len = 1024)
{
    return RDSAssembler::startTask(core_id, priority, stack_words, bit_queue_len);
}

inline bool nextBit(uint8_t &bit)
{
    return RDSAssembler::nextBit(bit);
}

inline void setPI(uint16_t pi)
{
    RDSAssembler::setPI(pi);
}
inline void setPTY(uint8_t pty)
{
    RDSAssembler::setPTY(pty);
}
inline void setTP(bool tp)
{
    RDSAssembler::setTP(tp);
}
inline void setTA(bool ta)
{
    RDSAssembler::setTA(ta);
}
inline void setMS(bool music)
{
    RDSAssembler::setMS(music);
}
inline void setPS(const char *ps)
{
    RDSAssembler::setPS(ps);
}
inline void setRT(const char *rt)
{
    RDSAssembler::setRT(rt);
}
inline void setAF_FM(const float *freqs_mhz, size_t count)
{
    RDSAssembler::setAF_FM(freqs_mhz, count);
}
inline void setClock(int year, int month, int day, int hour, int minute, int8_t offset_half_hours)
{
    RDSAssembler::setClock(year, month, day, hour, minute, offset_half_hours);
}
} // namespace RDSAssembler_NS

// =====================================================================================
//                                END OF FILE
// =====================================================================================
