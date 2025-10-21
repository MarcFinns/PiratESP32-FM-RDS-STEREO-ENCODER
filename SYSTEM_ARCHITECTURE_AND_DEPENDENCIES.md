# System Architecture and Dependencies

## Overview

The PiratESP32 FM RDS Stereo Encoder implements a sophisticated real-time audio processing system on the ESP32 dual-core microcontroller. The architecture leverages FreeRTOS for concurrent task execution, employs an Inversion of Control (IoC) container pattern for dependency management, and enforces strict initialization ordering to ensure system stability.

## System Architecture

### Dual-Core Task Allocation

The ESP32's dual-core architecture is exploited to maximize real-time performance. Core assignments are configurable in `Config.h` and applied at runtime by the SystemContext when each task is created.

- **Core 0 (PRO_CPU, typical default)**: Real-time audio processing
  - DSP Pipeline (audio input, stereo processing, FM modulation, RDS injection)

- **Core 1 (APP_CPU, typical default)**: I/O and background tasks
  - Console (serial CLI owner + diagnostics)
  - VU Meter (level monitoring, display updates)
  - RDS Assembler (metadata formatting, timing)

At startup, each module logs "… running on Core X" so you can verify the actual pinning after editing `Config.h`.

### Priority Strategy

FreeRTOS task priorities range from 0 (lowest) to 25 (highest). The system uses the following priority allocation:

| Task | Priority | Core | Rationale |
|------|----------|------|-----------|
| DSP Pipeline | 6 | 0 | Highest priority for real-time audio processing with <1ms latency requirements |
| RDS Assembler | 5 | 1 | Time-sensitive RDS data formatting (57kHz subcarrier timing) |
| VU Meter | 4 | 1 | Regular level monitoring without audio dropouts |
| Console | 3 | 1 | Background diagnostic output, lowest priority to avoid blocking |

**Note**: Priority 6 is the highest used to reserve headroom for potential FreeRTOS system tasks and interrupt handlers.

## Dependency Management

### SystemContext IoC Container

The system implements an Inversion of Control (IoC) container pattern via the `SystemContext` class. This centralizes module lifecycle management and enforces initialization ordering.

**Key Responsibilities**:
- Hardware driver initialization (I2S, SPI, GPIO)
- Service instantiation with dependency injection
- Startup sequence coordination
- Resource ownership and cleanup
- Shared state management

**Pattern Benefits**:
- Single source of truth for system configuration
- Compile-time dependency resolution
- Prevents circular dependencies
- Simplifies testing and mocking
- Clear module boundaries

### Dependency Graph

```
Hardware Layer (initialized first)
    |
    +-- I2S Driver (audio I/O)
    +-- SPI Driver (display, optional)
    +-- GPIO (controls, LEDs)
    |
    v
Console Service
    |
    v
VU Meter Service
    |   |
    |   +-- Requires: Console (for debug output)
    v
RDS Assembler Service
    |   |
    |   +-- Requires: Console (for diagnostics)
    v
DSP Pipeline
    |
    +-- Requires: Console (for performance metrics)
    +-- Requires: VU Meter (for level monitoring)
    +-- Requires: RDS Assembler (for metadata injection)
```

### Dependency Injection

Dependencies are injected via constructor parameters, ensuring explicit coupling:

```cpp
// Example: VU Meter depends on Console
VUMeter::VUMeter(Console* console, const VUConfig& config)
    : console_(console), config_(config) {
    // Validation
    if (!console_) {
        // Cannot proceed without console
    }
}

// Example: DSP Pipeline depends on multiple services
DSPPipeline::DSPPipeline(
    Console* console,
    VUMeter* vuMeter,
    RDSAssembler* rdsAssembler,
    const DSPConfig& config)
    : console_(console),
      vuMeter_(vuMeter),
      rdsAssembler_(rdsAssembler),
      config_(config) {
    // All dependencies validated before use
}
```

## Initialization Sequence

The startup sequence is carefully orchestrated to prevent race conditions and ensure all dependencies are available before tasks start:

### Phase 1: Hardware Initialization

1. **GPIO Configuration**
   - Configure input pins (buttons, encoders)
   - Configure output pins (LEDs, control signals)
   - Set initial states

2. **I2S Driver Setup**
   - Configure audio codec parameters (sample rate, bit depth)
   - Allocate DMA buffers (typically 4-8 buffers of 64-128 samples)
   - Start I2S peripheral in master mode

3. **SPI Driver Setup** (if display enabled)
   - Configure SPI bus for display controller
   - Initialize display hardware

**Critical Point**: Hardware must be fully initialized before any service tasks start, as hardware access during initialization can cause crashes.

### Phase 2: Service Initialization

Services are initialized in dependency order:

1. **Console Service**
   ```cpp
   Console* console = new Console();
   console->init();  // Start serial output, create task on Core 1
   ```
   - Creates FreeRTOS task on Core 1, Priority 3
   - Initializes queue (depth: 32 messages)
   - Begins serial output thread

2. **VU Meter Service**
   ```cpp
   vuMeter = new VUMeter(console, config.vuConfig);
   vuMeter->init();  // Create task on Core 1
   ```
   - Depends on: Console
   - Creates FreeRTOS task on Core 1, Priority 4
   - Initializes mailbox queue (depth: 1 message)
   - Begins level monitoring thread

3. **RDS Assembler Service**
   ```cpp
   rdsAssembler = new RDSAssembler(console, config.rdsConfig);
   rdsAssembler->init();  // Create task on Core 1
   ```
   - Depends on: Console
   - Creates FreeRTOS task on Core 1, Priority 5
   - Initializes FIFO queue (depth: 16 messages)
   - Begins RDS data formatting thread

4. **DSP Pipeline**
   ```cpp
   dspPipeline = new DSPPipeline(console, vuMeter, rdsAssembler, config.dspConfig);
   dspPipeline->init();  // Create task on Core 0
   ```
   - Depends on: Console, VU Meter, RDS Assembler
   - Creates FreeRTOS task on Core 0, Priority 6 (highest)
   - Initializes audio buffers
   - Begins real-time audio processing loop

### Phase 3: System Start

Once all services are initialized:

```cpp
void SystemContext::start() {
    console->start();        // Unblock console task
    vuMeter->start();       // Unblock VU meter task
    rdsAssembler->start();  // Unblock RDS task
    dspPipeline->start();   // Unblock DSP task (starts audio flow)
}
```

Tasks are created in a "suspended" or "waiting" state during `init()` and only released during `start()` to ensure the complete system is ready before processing begins.

## Task Lifecycle

### Task Creation

All tasks follow a common lifecycle pattern:

```cpp
void Service::init() {
    // 1. Allocate resources (buffers, queues)
    queue_ = xQueueCreate(queueDepth, sizeof(Message));

    // 2. Initialize state variables
    state_ = STATE_IDLE;

    // 3. Create FreeRTOS task
    xTaskCreatePinnedToCore(
        taskFunction,      // Task entry point
        taskName,          // Name for debugging
        stackSize,         // Stack size in words
        this,              // Parameter (usually 'this' pointer)
        priority,          // Task priority
        &taskHandle_,      // Handle for control
        coreId             // CPU core affinity
    );
}
```

### Task Execution

Each task runs an infinite loop:

```cpp
void Service::taskFunction(void* param) {
    Service* service = static_cast<Service*>(param);

    // Wait for start signal
    service->waitForStart();

    while (true) {
        // Process work
        service->processLoop();

        // Yield if no work available
        if (!service->hasWork()) {
            taskYIELD();  // Or vTaskDelay(1) for periodic tasks
        }
    }
}
```

### Task Termination

Tasks are long-lived and typically run for the entire system lifetime. Shutdown is handled by:

```cpp
void SystemContext::shutdown() {
    // Stop tasks in reverse dependency order
    dspPipeline->stop();      // Stop audio first
    rdsAssembler->stop();
    vuMeter->stop();
    console->stop();

    // Clean up resources
    delete dspPipeline;
    delete rdsAssembler;
    delete vuMeter;
    delete console;
}
```

## Queue Semantics

Different modules use different queue semantics based on their requirements:

### Console Log Queue: FIFO with Drop-on-Overflow

```cpp
QueueHandle_t logQueue = xQueueCreate(32, sizeof(LogMessage));

// Producer (any task)
if (xQueueSend(logQueue, &msg, 0) != pdTRUE) {
    // Drop message if queue full (don't block)
    droppedMessageCount++;
}

// Consumer (console task)
LogMessage msg;
if (xQueueReceive(logQueue, &msg, portMAX_DELAY) == pdTRUE) {
    Serial.println(msg.text);
}
```

**Rationale**: Logging should never block high-priority tasks. If the queue is full, drop messages rather than wait.

### VU Meter Queue: Mailbox (Latest Value Only)

```cpp
QueueHandle_t vuQueue = xQueueCreate(1, sizeof(VULevel));

// Producer (DSP pipeline)
VULevel level = {leftPeak, rightPeak};
xQueueOverwrite(vuQueue, &level);  // Always succeeds, overwrites old value

// Consumer (VU meter task)
VULevel level;
if (xQueueReceive(vuQueue, &level, 0) == pdTRUE) {
    updateDisplay(level);
}
```

**Rationale**: Only the latest level is relevant. Old values are discarded automatically.

### RDS Queue: FIFO with Blocking

```cpp
QueueHandle_t rdsQueue = xQueueCreate(16, sizeof(RDSData));

// Producer (RDS assembler)
RDSData data = assembleRDSGroup();
xQueueSend(rdsQueue, &data, portMAX_DELAY);  // Block if full

// Consumer (DSP pipeline)
RDSData data;
if (xQueueReceive(rdsQueue, &data, 0) == pdTRUE) {
    modulateRDS(data);
}
```

**Rationale**: RDS data must be delivered in order without loss. The assembler can afford to block briefly if the DSP is consuming slowly.

## Error Handling

### Initialization Failures

```cpp
bool SystemContext::init() {
    if (!initHardware()) {
        console->error("Hardware init failed");
        return false;
    }

    if (!console->init()) {
        Serial.println("FATAL: Console init failed");
        return false;
    }

    if (!vuMeter->init()) {
        console->error("VU Meter init failed");
        return false;
    }

    // ... etc

    return true;
}
```

If initialization fails, the system halts rather than operating in a degraded state.

### Runtime Failures

Runtime errors are logged and may trigger fallback behavior:

```cpp
void DSPPipeline::processAudio() {
    if (!i2sReadSamples(buffer, bufferSize)) {
        console->error("I2S read failed, using silence");
        memset(buffer, 0, bufferSize);
    }

    // Continue processing with fallback data
}
```

### Watchdog Protection

Critical tasks feed the watchdog timer:

```cpp
void DSPPipeline::processLoop() {
    while (true) {
        processAudio();
        esp_task_wdt_reset();  // Feed watchdog
    }
}
```

If the DSP task hangs, the watchdog triggers a system reset.

## Configuration

### Compile-Time Configuration

Many parameters are set at compile time for optimal performance:

```cpp
// config.h
#define SAMPLE_RATE 48000
#define BUFFER_SIZE 64
#define DSP_CORE 0
#define IO_CORE 1
#define DSP_PRIORITY 6
#define RDS_PRIORITY 5
#define VU_PRIORITY 4
#define LOG_PRIORITY 3
```

### Runtime Configuration

Some settings can be adjusted at runtime via the `SystemContext`:

```cpp
struct SystemConfig {
    uint32_t sampleRate;
    uint8_t logLevel;
    bool enableRDS;
    bool enableVUMeter;
    // ...
};

SystemContext context(config);
context.init();
context.start();
```

## Best Practices

### Adding a New Service

To add a new service to the system:

1. **Define dependencies**: What services does it need?
2. **Choose core and priority**: Real-time? → Core 0. I/O? → Core 1.
3. **Implement lifecycle**: `init()`, `start()`, `stop()` methods
4. **Register with SystemContext**: Add to initialization sequence
5. **Update dependency graph**: Ensure correct initialization order

### Memory Management

- **Static allocation preferred**: Use fixed-size buffers when possible
- **RTOS-aware allocation**: Use `pvPortMalloc()` for FreeRTOS compatibility
- **No dynamic deallocation in tasks**: Avoid `free()` in high-frequency loops
- **Stack size tuning**: Monitor stack usage with `uxTaskGetStackHighWaterMark()`

### Debugging

- **Task monitoring**: Use `vTaskList()` to inspect all tasks
- **Queue monitoring**: Use `uxQueueMessagesWaiting()` to detect bottlenecks
- **Performance profiling**: Use `esp_timer_get_time()` for microsecond timing
- **Console levels**: Use DEBUG level for verbose output, ERROR for critical issues

## Summary

The PiratESP32 system architecture achieves reliable real-time audio processing through:

- **Deterministic task allocation**: Core 0 for audio, Core 1 for I/O
- **Priority-based scheduling**: Highest priority for time-critical DSP
- **Dependency injection**: Clear module boundaries and testability
- **Ordered initialization**: Hardware → Console → VU → RDS → DSP
- **Appropriate queue semantics**: Drop, mailbox, or blocking based on requirements
- **Centralized lifecycle management**: SystemContext IoC container

This architecture ensures sub-millisecond audio latency, stable RDS encoding, and responsive user interface without conflicts or race conditions.
