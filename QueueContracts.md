# Queue Communication Contracts

## Overview

This document describes the inter-module communication patterns via FreeRTOS queues. Each queue has specific semantics designed for its use case. Understanding these contracts is critical for maintaining system stability and predictable behavior.

---

## Queue Contract Summary

| Module | Queue Name | Type | Size | Overflow Behavior | Reason |
|--------|-----------|------|------|-------------------|--------|
| **Log** | Logger queue | FIFO Drop | 64 msgs (10 KB) | Drop oldest message silently | Real-time, timestamp captures time anyway |
| **VUMeter** | Sample queue | Mailbox | 1 sample | Overwrite old sample | Only latest peak matters |
| **VUMeter** | Stats queue | Mailbox | 1 snapshot | Overwrite old snapshot | Only latest stats matter |
| **RDSAssembler** | Bit queue | FIFO Drop-oldest | 1024 bits | Drop oldest bit if full | Sequential RDS stream, recover sync by advancing |
| **DSP_pipeline** | (Internal only) | N/A | N/A | N/A | Processes synchronously |

---

## Detailed Queue Specifications

### 1. Log Module - Logger Queue

**Queue Type:** FIFO with drop-on-overflow

**Parameters:**
- Default size: 64 messages
- Message size: 160 bytes each (timestamp 8 bytes + level 1 byte + string 159 bytes)
- Total capacity: ~10 KB
- Timeout: 0 (non-blocking)

**Overflow Semantics:**
```cpp
bool Log::enqueueRaw(LogLevel level, const char *msg) {
    if (xQueueSend(queue_, &entry, 0) != pdTRUE) {
        ++dropped_count_;  // Counter incremented
        return false;      // Message dropped silently
    }
    return true;
}
```

**Why This Design:**
- Logging is diagnostic, not critical for audio processing
- Non-blocking operation essential for real-time code
- Dropped messages are rare; timestamp allows detection of loss
- Freshness more important than completeness
- Serial I/O latency means older messages already stale

**Tradeoffs:**
- ✅ Never blocks real-time audio code
- ✅ Lightweight, minimal overhead
- ❌ Can lose messages under extreme load
- ❌ No buffering; cannot "catch up" later

**Usage Pattern:**
```cpp
// From DSP_pipeline or VUMeter (Core 1)
Log::enqueuef(LogLevel::ERROR, "I2S read failed: %d", error_code);
// Logger task asynchronously drains queue to Serial
```

---

### 2. VUMeter Module - Sample Queue

**Queue Type:** Mailbox (single-slot queue with overwrite)

**Parameters:**
- Size: 1 sample (queue_len = 1)
- Sample size: 28 bytes (7 × 4-byte floats)
- Timeout: 0 (non-blocking)
- Behavior: Overwrite oldest message when full

**Overflow Semantics:**
```cpp
bool VUMeter::enqueueRaw(const VUSample& s) {
    if (uxQueueSpacesAvailable(queue_) == 0 && uxQueueMessagesWaiting(queue_) == 1) {
        xQueueOverwrite(queue_, (void *)&s);  // Overwrite the single slot
        sample_overflow_count_++;
        return true;  // Overwrites always succeed
    }
    if (xQueueSend(queue_, &s, 0) != pdTRUE) {
        sample_overflow_count_++;
        return false;
    }
    return true;
}
```

**Why This Design:**
- VU meter only needs latest peak levels
- Audio levels change every ~5 ms, but display updates at ~50 FPS
- Older samples are irrelevant by the time display renders
- Single-slot mailbox keeps memory usage minimal
- Overwrite semantics ensure display always shows most recent data
- No stale data displayed

**Tradeoffs:**
- ✅ Minimal memory (1 sample = 28 bytes)
- ✅ Always non-blocking, never waits
- ✅ Display always sees latest level information
- ❌ Cannot track historical trends
- ❌ Intermediate samples lost
- ❌ No queueing of rapid level changes

**Usage Pattern:**
```cpp
// From DSP_pipeline (Core 0) every 5 ms
VUSample s;
s.l_peak = left_peak;
s.r_peak = right_peak;
s.l_dbfs = 20.0f * log10f(left_peak);
VUMeter::enqueue(s);  // If queue full, overwrites old sample
```

---

### 3. VUMeter Module - Stats Queue

**Queue Type:** Mailbox (single-slot queue with overwrite)

**Parameters:**
- Size: 1 snapshot (queue_len = 1)
- Snapshot size: ~140 bytes (21 fields)
- Timeout: 0 (non-blocking)
- Behavior: Overwrite oldest message when full

**Overflow Semantics:**
```cpp
bool VUMeter::enqueueStatsRaw(const VUStatsSnapshot& s) {
    // Same mailbox pattern as sample queue
    if (uxQueueSpacesAvailable(stats_queue_) == 0 && uxQueueMessagesWaiting(stats_queue_) == 1) {
        xQueueOverwrite(stats_queue_, (void *)&s);
        stats_overflow_count_++;
        if (stats_overflow_count_ == 1 || (stats_overflow_count_ % 100 == 0)) {
            ErrorHandler::logWarning("...", "stats queue overflow");
        }
        return true;
    }
    // ... normal xQueueSend ...
}
```

**Why This Design:**
- Status panel updates at 1 Hz
- CPU/heap/stack metrics valid for ~1 second
- Older snapshots immediately stale
- Single slot keeps display panel responsive
- Overwrite ensures always displaying latest system state

**Tradeoffs:**
- ✅ Minimal memory overhead
- ✅ Always non-blocking
- ✅ Display shows current system state
- ❌ Cannot observe state transitions
- ❌ Cannot detect transient spikes

**Usage Pattern:**
```cpp
// From DSP_pipeline (Core 0) every 1 second
VUStatsSnapshot snap;
snap.cpu_usage = calculate_cpu_usage();
snap.heap_free = ESP.getFreeHeap();
snap.uptime_s = (esp_timer_get_time() - start_time_us) / 1000000ULL;
VUMeter::enqueueStats(snap);  // If queue full, overwrites old snapshot
```

---

### 4. RDSAssembler Module - Bit Queue

**Queue Type:** FIFO with drop-oldest-on-overflow

**Parameters:**
- Size: 1024 bits (128 bytes)
- Element size: 1 byte (uint8_t bit value)
- Timeout: 0 (non-blocking)
- Overflow: Drop oldest bit to make room for new bits

**Overflow Semantics:**
```cpp
auto enqueueBlock = [&](uint16_t info, uint16_t offset) {
    uint16_t cw = crc10(info) ^ (offset & 0x3FFu);
    uint32_t block26 = ((uint32_t)info << 10) | cw;

    for (int i = 25; i >= 0; --i) {
        uint8_t bit = (block26 >> i) & 1u;
        if (bit_queue_) {
            // If queue is full, drop oldest bit to make room
            if (uxQueueSpacesAvailable(bit_queue_) == 0 && uxQueueMessagesWaiting(bit_queue_) >= 1) {
                uint8_t dummy;
                xQueueReceive(bit_queue_, &dummy, 0);  // Drop oldest
            }
            xQueueSend(bit_queue_, &bit, 0);
        }
    }
};
```

**Why This Design:**
- RDS bitstream is continuous; bits must be sequential
- Buffer allows DSP pipeline (Core 0) to read bits asynchronously from RDS assembler (Core 1)
- Cannot drop arbitrary bits; must maintain sequence
- When queue is full, oldest bits are least critical (already transmitted)
- By dropping oldest bits, system recovers synchronization
- 1024-bit buffer = ~860 ms buffer at 1187.5 bps

**Tradeoffs:**
- ✅ Maintains RDS bit sequence integrity
- ✅ Allows asynchronous producer/consumer
- ✅ Large buffer (860 ms) provides timing slack
- ✅ Never blocks real-time code
- ⚠️ When full, drops oldest transmitted bits (acceptable recovery)
- ❌ Under extreme load, consumer may fall behind

**Usage Pattern:**
```cpp
// Producer: RDSAssembler (Core 1) generates bits continuously
// At ~1187.5 bps, enqueues bits as they're encoded

// Consumer: DSP_pipeline (Core 0) reads bits at 57 kHz
uint8_t rds_bit;
if (RDSAssembler::nextBit(rds_bit)) {
    // Use bit in 57 kHz RDS carrier modulation
    rds_buffer_[i] = rds_bit ? +RDS_AMP : -RDS_AMP;
}
```

---

## Key Design Principles

### 1. **Non-Blocking Operations**
All queues use timeout = 0 (non-blocking):
```cpp
xQueueSend(queue, &msg, 0);        // Returns immediately
xQueueReceive(queue, &msg, 0);     // Returns immediately
```
This ensures real-time code never waits for queue operations.

### 2. **Real-Time vs. Non-Real-Time Tradeoffs**

**Real-Time Path (Core 0 - Audio):**
- DSP_pipeline: Must complete in < 1.33 ms per block
- Cannot wait on queues
- Cannot afford dynamic memory allocation
- Uses non-blocking sends only

**Non-Real-Time Path (Core 1 - Display/Logger):**
- Can wait on queues to consume messages
- Display updates at 50 FPS (20 ms interval)
- Can perform I/O operations

### 3. **Overflow Strategies**

| Strategy | Used By | Rationale |
|----------|---------|-----------|
| **Drop Oldest** | Log | Logging is diagnostic; timestamp indicates loss |
| **Overwrite** | VUMeter | Only latest value matters for display |
| **Drop Oldest** | RDSAssembler | Recover sync by advancing through bit stream |

### 4. **Memory Efficiency**

```
Log:          64 msgs × 160 bytes = 10 KB
VUMeter:      1 sample × 28 bytes = 28 bytes
Stats:        1 snapshot × 140 bytes = 140 bytes
RDSAssembler: 1024 bits × 1 byte = 1 KB
─────────────────────────────────────────
Total:        ~11.2 KB for all queues
```

---

## Error Detection and Logging

All queues track overflow events via error counters:

```cpp
// VUMeter overflow tracking
sample_overflow_count_++;           // Always incremented
if (!sample_overflow_logged_) {
    sample_overflow_logged_ = true; // Log first occurrence only
    ErrorHandler::logError(...);
}

// Status panel displays
"Loops: %u  Errors: %u  Overflow: %u"
// Overflow = sample_overflow_count_ + stats_overflow_count_
```

---

## Testing and Validation

### Stress Test Scenarios

1. **Log overflow:** Trigger with rapid logging
   - Expected: First overflow logged, subsequent drops silent
   - Metric: `dropped_count_` increments

2. **VUMeter sample overflow:** Send samples faster than display updates
   - Expected: Display shows latest sample only
   - Metric: `sample_overflow_count_` increments

3. **RDSAssembler queue full:** Consumer slower than producer
   - Expected: Oldest bits dropped, sync maintained
   - Metric: `bit_overflow_count_` increments

### Monitoring in Production

Status panel displays aggregate overflow counter:
```
Loops: 3600  Errors: 0  Overflow: 2
              ↑          ↑         ↑
           loops    DSP errors   VU overflows
```

---

## Design Rationale Summary

| Module | Queue Type | Why | Consequence |
|--------|-----------|-----|-------------|
| **Log** | FIFO Drop | Diagnostic, freshness critical | Rare message loss acceptable |
| **VUMeter** | Mailbox | Display only needs current state | Historical data lost |
| **RDSAssembler** | FIFO Drop | Maintain bit sequence | Oldest bits discarded |

This design ensures:
- ✅ Real-time code never blocks
- ✅ Memory usage predictable and minimal
- ✅ Graceful degradation under load
- ✅ Clear communication patterns
- ✅ Visible error tracking

