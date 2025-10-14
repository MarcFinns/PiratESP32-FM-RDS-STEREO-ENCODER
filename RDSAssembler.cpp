/*
 * =====================================================================================
 *
 *                              RDS Assembler (Core 1)
 *
 * =====================================================================================
 */
#include "RDSAssembler.h"

#include <freertos/queue.h>
#include <freertos/task.h>

namespace RDSAssembler
{
// Single‑producer/single‑consumer FIFO of bits for the audio core.
static QueueHandle_t g_bit_queue = nullptr;
static TaskHandle_t g_task = nullptr;

// ===================== RDS builder state =====================
static uint16_t g_pi = 0x1234;     // Program Identification (default)
static uint8_t  g_pty = 0;         // Program Type (0..31)
static bool     g_tp  = false;     // Traffic Program
static bool     g_ta  = false;     // Traffic Announcement
static bool     g_ms  = true;      // Music (true) / Speech (false)

static char     g_ps[8]  = { 'E','S','P','3','2',' ','F','M' };
static char     g_rt[64] = { 'H','e','l','l','o',' ','R','D','S',' ','o','n',' ','E','S','P','3','2','!',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
                              ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
static bool     g_rt_ab  = false;  // Text A/B flag (toggle to force RT refresh on receivers)

// ============ CRC10 and offset words ============
// CRC polynomial: x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 (0x5B9)
static inline uint16_t crc10(uint16_t info)
{
    uint32_t reg = (uint32_t)info << 10; // append 10 zero bits
    const uint32_t poly = 0x5B9u;        // 10-bit poly
    for (int i = 25; i >= 10; --i)
    {
        if (reg & (1u << i))
        {
            reg ^= (poly << (i - 10));
        }
    }
    return (uint16_t)(reg & 0x3FFu);
}

// 10-bit offset words for blocks A, B, C, C' (version B), and D.
// Values per EN 50067; used to XOR the CRC remainder to form the checkword.
static constexpr uint16_t kOffsetA  = 0x0FC; // 252
static constexpr uint16_t kOffsetB  = 0x198; // 408
static constexpr uint16_t kOffsetC  = 0x168; // 360
static constexpr uint16_t kOffsetD  = 0x1B4; // 436
static constexpr uint16_t kOffsetCp = 0x1CC; // 460 (C' for version B)

// Serialize a 26-bit block (info, offset) MSB‑first into the bit FIFO.
static inline void enqueueBlock(uint16_t info, uint16_t offset)
{
    uint16_t cw = crc10(info) ^ (offset & 0x3FFu);
    uint32_t block26 = ((uint32_t)info << 10) | cw;
    for (int i = 25; i >= 0; --i)
    {
        uint8_t bit = (block26 >> i) & 1u;
        if (g_bit_queue)
        {
            if (uxQueueSpacesAvailable(g_bit_queue) == 0 && uxQueueMessagesWaiting(g_bit_queue) >= 1)
            {
                uint8_t dummy;
                xQueueReceive(g_bit_queue, &dummy, 0);
            }
            xQueueSend(g_bit_queue, &bit, 0);
        }
    }
}

// Build and enqueue one Group 2A (RadioText 64) for the given segment address (0..15).
static void buildGroup2A(uint8_t seg)
{
    // Block A: PI
    enqueueBlock(g_pi, kOffsetA);

    // Block B: group code + flags + segment address
    // Bits: 15..12 = 0b0010 (group 2), 11 = 0 (A), 10=TP, 9..5=PTY, 4=AB, 3..0=segment
    uint16_t b = 0;
    b |= (2u << 12);
    // version A -> bit 11 = 0
    b |= (g_tp ? 1u : 0u) << 10;
    b |= ((uint16_t)(g_pty & 0x1Fu)) << 5;
    b |= (g_rt_ab ? 1u : 0u) << 4;
    b |= (seg & 0x0Fu);
    enqueueBlock(b, kOffsetB);

    // Block C and D: 4 chars total, big‑endian pairs (char_hi<<8 | char_lo)
    uint8_t i0 = seg * 4;
    uint16_t c = ((uint16_t)(uint8_t)g_rt[i0 + 0] << 8) | (uint16_t)(uint8_t)g_rt[i0 + 1];
    uint16_t d = ((uint16_t)(uint8_t)g_rt[i0 + 2] << 8) | (uint16_t)(uint8_t)g_rt[i0 + 3];
    enqueueBlock(c, kOffsetC);
    enqueueBlock(d, kOffsetD);
}

// Build and enqueue one Group 0A (Program Service name) for the given segment (0..3)
// Build and enqueue one Group 0B (Program Service name) for the given segment (0..3)
// Version B is used to avoid AF handling; PS chars are carried in block D.
static void buildGroup0B(uint8_t seg)
{
    // Block A: PI
    enqueueBlock(g_pi, kOffsetA);

    // Block B: group 0B flags + DI bit + segment (lower 2 bits)
    // Bits: 15..12 = 0b0000, 11=1 (B), 10=TP, 9..5=PTY, 4=TA, 3=MS, 2=DI, 1..0=seg
    uint16_t b = 0;
    // group 0 -> top 4 bits are 0
    b |= 1u << 11; // Version B
    b |= (g_tp ? 1u : 0u) << 10;
    b |= ((uint16_t)(g_pty & 0x1Fu)) << 5;
    b |= (g_ta ? 1u : 0u) << 4;
    b |= (g_ms ? 1u : 0u) << 3;
    // DI: place a bit that can be tied to segment (simple: DI=seg==0)
    b |= ((seg == 0) ? 1u : 0u) << 2;
    b |= (seg & 0x03u);
    enqueueBlock(b, kOffsetB);

    // Block C′: no AF, place filler (e.g., spaces 0x2020 or zero)
    // Use kOffsetCp for version B
    uint16_t cprime = 0x2020u; // two spaces
    enqueueBlock(cprime, kOffsetCp);

    // Block D: two PS characters
    uint8_t i0 = seg * 2;
    uint16_t d = ((uint16_t)(uint8_t)g_ps[i0 + 0] << 8) | (uint16_t)(uint8_t)g_ps[i0 + 1];
    enqueueBlock(d, kOffsetD);
}

bool nextBit(uint8_t &bit)
{
    if (!g_bit_queue)
    {
        return false;
    }
    return xQueueReceive(g_bit_queue, &bit, 0) == pdTRUE;
}

// Simple PRBS generator placeholder: produces pseudo‑random bits at ~1187.5 bps.
// Replace with a proper RDS group scheduler + CRC/checkwords later.
static void assembler_task(void *arg)
{
    (void)arg;

    // Simple time‑paced serializer at ~1187.5 bps
    const TickType_t tick_1ms = pdMS_TO_TICKS(1);
    uint32_t accu_us = 0;
    const uint32_t bit_us = 842; // approximate

    // Group rotation indices
    uint8_t seg_ps = 0;  // 0..3
    uint8_t seg_rt = 0;  // 0..15

    // Bit buffer for current 26‑bit block serialization
    // We build whole groups on demand by calling buildGroupXX, which enqueues bits directly.

    for (;;)
    {
        vTaskDelay(tick_1ms);
        accu_us += 1000;

        while (accu_us >= bit_us)
        {
            accu_us -= bit_us;

            // If queue has room to absorb full group bursts, we can push a group when low level
            // For simplicity, push one bit per pacing; but if the queue gets low, prefill by emitting a group.
            if (uxQueueMessagesWaiting(g_bit_queue) < 26)
            {
                // Alternate 0A and 2A to keep PS fresh and RT flowing
                if ((seg_rt & 0x01) == 0)
                {
                    buildGroup0B(seg_ps);
                    seg_ps = (uint8_t)((seg_ps + 1) & 0x03);
                }
                else
                {
                    buildGroup2A(seg_rt);
                    seg_rt = (uint8_t)((seg_rt + 1) & 0x0F);
                }
            }

            // Emit one bit from the queue at this pacing tick (acts as a drain limiter)
            // If the queue is empty, synth will read idle (false) via nextBit()
            // No need to dequeue here; the consumer dequeues bits. We just keep filling.
            // (We could also do nothing here since we already pace group emission via level check.)
        }
    }
}

bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words, std::size_t bit_queue_len)
{
    if (g_task)
    {
        return true;
    }
    if (bit_queue_len == 0)
    {
        bit_queue_len = 1024;
    }
    g_bit_queue = xQueueCreate((UBaseType_t)bit_queue_len, sizeof(uint8_t));
    if (!g_bit_queue)
    {
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        assembler_task,
        "rds_asm",
        stack_words,
        nullptr,
        priority,
        &g_task,
        core_id);

    return ok == pdPASS;
}

void setPS(const char *ps)
{
    if (!ps) return;
    // Copy exactly 8 chars, pad with spaces
    for (int i = 0; i < 8; ++i)
    {
        char c = ps[i];
        if (c == '\0') c = ' ';
        g_ps[i] = c;
    }
}

void setRT(const char *rt)
{
    if (!rt) return;
    // Copy up to 64 chars, pad with spaces
    int i = 0;
    for (; i < 64 && rt[i] != '\0'; ++i)
    {
        g_rt[i] = rt[i];
    }
    for (; i < 64; ++i)
    {
        g_rt[i] = ' ';
    }
    // Toggle A/B flag to force RT refresh on receivers
    g_rt_ab = !g_rt_ab;
}

void setPI(uint16_t pi) { g_pi = pi; }
void setPTY(uint8_t pty) { g_pty = (uint8_t)(pty & 0x1Fu); }
void setTP(bool tp) { g_tp = tp; }
void setTA(bool ta) { g_ta = ta; }
void setMS(bool music) { g_ms = music; }
} // namespace RDSAssembler
