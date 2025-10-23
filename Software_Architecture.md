# Software Architecture

This document explains how the system is structured: tasks, queues, modules, lifecycle patterns, and dependency management.

## Task Model (ESP32‑S3)

The system uses a dual-core architecture to achieve real-time audio processing and responsive I/O:

**Core 0 (PRO_CPU) — Real-Time Audio**
- `DSP_pipeline` (priority 6, highest)
- Processes 64-sample blocks (~1.33 ms) deterministically
- Handles I2S RX/TX, stereo MPX synthesis, RDS injection

**Core 1 (APP_CPU) — I/O and Services**
- `Console` (priority 3) — serial CLI + log drain
- `DisplayManager` (priority 4) — VU meter + TFT UI
- `RDSAssembler` (priority 5) — RDS group scheduling and bit generation

### Priority Strategy

FreeRTOS priorities (0–25, higher = more urgent):

| Task | Priority | Core | Rationale |
|------|----------|------|-----------|
| DSP Pipeline | 6 | 0 | Sub-millisecond audio latency |
| RDS Assembler | 5 | 1 | Time-sensitive RDS at 57 kHz |
| DisplayManager | 4 | 1 | Regular UI updates, no audio impact |
| Console | 3 | 1 | Background diagnostics, lowest priority |

All cores and priorities are configurable via `Config.h` and verified at startup (each module logs "… running on Core X").

## Lifecycle Pattern

All modules implement a common lifecycle pattern:

```cpp
class TaskBaseClass {
public:
    // Initialization: allocate resources, setup state
    virtual bool begin() = 0;

    // Main processing: non-blocking work iteration
    virtual void process() = 0;

    // Cleanup: release resources
    virtual void shutdown() = 0;
};
```

**Startup Sequence:**
1. Hardware initialization (I2S, SPI, GPIO)
2. Console service start → other services depend on it
3. DisplayManager start (depends on Console)
4. RDSAssembler start (depends on Console)
5. DSP_pipeline start (depends on Console, DisplayManager, RDSAssembler)

**Key Rules:**
- All modules start in suspended/waiting state
- `begin()` allocates queues and resources
- Tasks only created if previous modules succeeded
- On error, system halts (no degraded operation)
- Shutdown reverses startup order

## Dependency Injection & Ownership

The system uses constructor-based dependency injection for clarity and testability:

```cpp
// Example: DSP_pipeline depends on hardware driver and services
DSP_pipeline::DSP_pipeline(
    IHardwareDriver* hw_driver,
    Console* console,
    DisplayManager* vu_meter,
    RDSAssembler* rds_assembler)
    : hw_driver_(hw_driver),
      console_(console),
      vu_meter_(vu_meter),
      rds_assembler_(rds_assembler) {
    // All dependencies validated
}
```

**Ownership Model:**
- Console owns Serial RX/TX; others never print directly (use queue instead)
- DSP_pipeline owns I2S hardware driver instance
- DisplayManager owns TFT/SPI peripherals
- RDSAssembler owns RDS bit generation logic
- SystemContext (IoC container) owns lifecycle of all services

## Queues and Semantics

Different queue types serve different roles; all use non-blocking operations (timeout=0) to ensure real-time code never waits.

### Console Log Queue
**Type:** FIFO with drop-oldest
**Capacity:** 64 messages (~10 KB)
**Overflow behavior:** Drop oldest message silently (timestamp indicates loss)
**Rationale:** Logging is diagnostic; never block audio code

**Pattern:**
```cpp
// Producer (any task, including real-time code)
Console::enqueuef(LogLevel::ERROR, "I2S error: %d", code);  // Non-blocking

// Consumer (Console task drains queue to Serial)
```

### DisplayManager VU Queue
**Type:** Mailbox (single-slot, overwrite)
**Capacity:** 1 sample (28 bytes) + 1 snapshot (140 bytes)
**Overflow behavior:** Overwrite old value with new one
**Rationale:** Only latest peak/stats matter; display updates at ~50 FPS

**Pattern:**
```cpp
// Producer (DSP_pipeline every ~5 ms)
VUSample s = {left_peak, right_peak, ...};
DisplayManager::enqueueSample(s);  // Overwrites if full

// Consumer (DisplayManager task renders display)
```

### RDSAssembler Bit Queue
**Type:** FIFO with drop-oldest
**Capacity:** 1024 bits (~860 ms @ 1187.5 bps)
**Overflow behavior:** Drop oldest bit to make room (recovers synchronization)
**Rationale:** Bits must be sequential; buffer decouples Core 1 producer from Core 0 consumer

**Pattern:**
```cpp
// Producer (RDSAssembler encodes bits continuously)
for (int i = 25; i >= 0; --i) {
    uint8_t bit = (block26 >> i) & 1;
    RDSAssembler::enqueueBit(bit);  // Non-blocking
}

// Consumer (DSP_pipeline reads bits for 57 kHz modulation)
uint8_t rds_bit;
if (RDSAssembler::nextBit(rds_bit)) {
    rds_samples[i] = rds_bit ? +RDS_AMP : -RDS_AMP;
}
```

### Queue Memory Footprint
```
Console Log:       64 msgs  × 160 bytes = 10 KB
DisplayManager:    1 sample × 28 bytes  + 1 snapshot × 140 bytes = 168 bytes
RDSAssembler:      1024 bits × 1 byte   = 1 KB
─────────────────────────────────────────────────────
Total:             ~11.2 KB for all inter-task communication
```

## Module Initialization Order

**Phase 1: Hardware Layer**
```cpp
GPIO configuration
I2S driver setup (ADC RX at 48 kHz, DAC TX at 192 kHz, MCLK master)
SPI driver setup (optional, for TFT display)
```

**Phase 2: Service Layer** (each module calls `begin()` → `startTask()`)
```cpp
1. Console (creates log queue, starts task on Core 1, priority 3)
2. DisplayManager (allocates buffers, starts task on Core 1, priority 4)
3. RDSAssembler (initializes bit FIFO, starts task on Core 1, priority 5)
4. DSP_pipeline (allocates DSP buffers, starts task on Core 0, priority 6)
```

**Phase 3: System Start**
- All tasks are created but suspended until `start()` is called
- `start()` unblocks each task in dependency order
- DSP_pipeline begins immediately processing audio

## Error Handling

**Initialization Errors** → System halts with error logged:
```cpp
if (!hardware_driver_->initialize()) {
    Console::enqueue(LogLevel::ERROR, "Hardware init failed!");
    return false;  // System stops
}
```

**Runtime Errors** → Log and degrade gracefully:
```cpp
if (!hardware_driver_->read(rx_buffer_, size, bytes_read)) {
    auto err = hardware_driver_->getErrorStatus();
    Console::enqueuef(LogLevel::ERROR, "I2S read failed: %d", err);
    memset(rx_buffer_, 0, size);  // Use silence instead
    // Continue processing
}
```

**Watchdog Protection** → Critical tasks feed watchdog timer:
```cpp
void DSP_pipeline::process() {
    processAudio();
    esp_task_wdt_reset();  // Reset watchdog
}
// If DSP task hangs >threshold, ESP32 resets automatically
```

## Design Patterns

### Singleton Pattern
Used for system-wide resources with single instance:
- `DSP_pipeline` — one audio engine
- `SystemContext` — one IoC container
- Module namespaces with static state

### Dependency Injection
Dependencies passed via constructors; enables:
- Hardware-independent business logic
- Easy mocking for unit tests
- Flexible module swapping

### Service Locator
Central access point via `SystemContext`:
```cpp
SystemContext& sys = SystemContext::getInstance();
DSP_pipeline* dsp = sys.getDSPPipeline();
IHardwareDriver* hw = sys.getHardwareDriver();
```

### Factory Pattern
Modules create FreeRTOS tasks with consistent interface:
```cpp
DSP_pipeline::startTask(core_id, priority, stack_words);
// Internally: creates instance → begin() → infinite process() loop
```

## Persistence (Profiles)

Configuration profiles are stored in NVS (Non-Volatile Storage):

**Namespace:** `conf`
**Keys:**
- `_list` — CSV of profile names
- `_active` — currently active profile name
- `p:<name>` — serialized config blob for profile `<name>`

**Runtime Interface:**
```cpp
SYST:CONF:SAVE <name>    // Save current settings as profile
SYST:CONF:LOAD <name>    // Load profile (applies all settings)
SYST:CONF:DEL <name>     // Delete profile
```

When a profile is loaded, all DSP/audio/RDS settings are applied immediately.

## Logging Policy

**Startup Phase (console task not yet running):**
- Logs printed directly to Serial for early diagnostics

**Runtime Phase (after `Console::markStartupComplete()`):**
- Logs queued to Console
- If `LogMuting::OFF` configured, applies selective silence policy
- High-priority audio code logs non-blocking via queue

## Performance Targets

- **DSP block latency:** 1.33 ms (64 samples @ 48 kHz input)
- **CPU headroom:** 20–30% (leave 70% free for other tasks)
- **Queue depth monitoring:** Overflow counters visible in status panel

## Diagrams (TODO)

- Task/queue interaction diagram with producers/consumers
- Startup and shutdown sequence flow
- Dependency graph visualization

