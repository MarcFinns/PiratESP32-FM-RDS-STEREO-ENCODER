/*
 * =====================================================================================
 *
 *                               RDS Assembler (Core 1)
 *
 * =====================================================================================
 *
 * Purpose
 *   Produces the RDS bitstream (1187.5 bps) on a non‑real‑time core. The audio core
 *   reads bits via a non‑blocking API and synthesizes the 57 kHz RDS injection
 *   synchronized to the 192 kHz sample clock.
 *
 * Design
 *   - Runs as a FreeRTOS task pinned to Core 1 (logger/display core)
 *   - Writes bits into a single‑producer/single‑consumer FreeRTOS queue
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

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>

namespace RDSAssembler
{
// =====================================================================================
//                                PUBLIC API (SKELETON)
// =====================================================================================
//
// The assembler is responsible for building a continuous RDS bitstream composed of
// 26-bit blocks (16-bit information + 10-bit checkword) grouped into 4-block groups.
// Typical groups:
//   - 0B (PS name):   static 8-char station name rotated over repeated groups (no AF)
//   - 2A (RadioText): 64-char text split into segments
//   - 4A (CT):        clock time/date (periodic)
//   - 15A (AF/EON):   alternative frequencies / other networks (optional)
//
// Each block’s checkword is offset-added by one of the four fixed offset words
// (A/B/C/C') as per EN 50067/IEC 62106.
//
// This assembler builds basic groups and provides a real-time bit FIFO. The default
// scheduler interleaves 0B (PS) and 2A (RT) with no AF support. Group 4A (CT) can
// be added later.

// Start the assembler task pinned to core_id, with the given priority and stack.
// bit_queue_len controls the depth of the bit FIFO; 1024 is ample for safety.
bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words, std::size_t bit_queue_len = 1024);

// Non‑blocking fetch of the next bit. Returns true if a bit was available.
bool nextBit(uint8_t &bit);

// Placeholders for future configurability (Program Service name and Radio Text).
// ===================== Builder Control APIs (to be implemented) =====================
// Set Program Identification code (PI)
void setPI(uint16_t pi);

// Set Program Type (PTY, 0..31)
void setPTY(uint8_t pty);

// Set Traffic Program (TP) and Traffic Announcement (TA) flags
void setTP(bool tp);
void setTA(bool ta);

// Set Music/Speech flag (true = Music, false = Speech)
void setMS(bool music);

// Set Program Service name (exactly 8 chars, padded with spaces if shorter)
void setPS(const char *ps);

// Set Radio Text (up to 64 chars, padded or truncated by the builder)
void setRT(const char *rt);

// Set AF list (FM VHF, Method A). freqs_mhz: array of MHz (e.g., 101.1f).
// Only 87.6–107.9 MHz are encoded (0.1 MHz step). Max 25 entries per spec.
void setAF_FM(const float *freqs_mhz, std::size_t count);

// Set Clock-Time (Group 4A). All parameters are local time (not UTC).
// offset_half_hours: local time offset from UTC in 30‑minute steps (e.g., +2h = +4)
void setClock(int year, int month, int day, int hour, int minute, int8_t offset_half_hours);

// (Sketch) Additional controls commonly used in RDS:
// - setPI(uint16_t pi)      : Program Identification
// - setPTY(uint8_t pty)     : Program Type
// - setTP(bool tp)          : Traffic Program flag
// - setTA(bool ta)          : Traffic Announcement flag
// - setMS(bool music)       : Music/Speech flag
// - setAF(const uint8_t *list, size_t n) : Alternative Frequencies
// - setCT(time_t utc)       : Clock Time (for group 4A)
// These can be added when implementing the group scheduler.

// ===================== Scheduler sketch (for implementation later) ==================
//
// 1) Maintain shared state:
//    - Current PS[8], RT[64], PI/PTY/TP/TA/MS, AF list, CT cache...
// 2) Periodically build groups into a small buffer (e.g., 50–100 groups ahead):
//    - Emit a rotation like: 0A, 2A, 0A, 2A, 0A, 4A, ... (tunable)
//    - For each block in the group:
//      * Assemble 16-bit information content
//      * Compute 10-bit checkword (CRC) and add appropriate offset word (A/B/C/C')
//      * Serialize 26 bits MSB->LSB into the bit FIFO
// 3) Push bits into the FIFO at a steady cadence (~1187.5 bps). If the bit FIFO
//    is near full, you can drop older bits (freshness wins) — the synth will keep
//    the waveform continuous by repeating idle bits.
// 4) Ensure all queue operations are non-blocking to avoid interacting with audio.
} // namespace RDSAssembler
