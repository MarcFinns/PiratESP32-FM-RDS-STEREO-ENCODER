# Architecture Standards

## Overview

This document defines the architectural patterns, conventions, and standards used throughout the PiratESP32-FM-RDS-STEREO-ENCODER project.

---

## 1. Design Patterns

### 1.1 Singleton Pattern

**Usage**: Single instance components that manage system-wide resources

**Examples**:
- `DSP_pipeline` - Main audio processing engine (one instance per system)
- `SystemContext` - Central IoC container (exactly one instance)
- Module namespaces with static state

**Implementation**:
```cpp
// Eager initialization (compile-time)
static DSP_pipeline s_instance(hardware_driver);
return s_instance.startTaskInstance(...);

// Lazy initialization (runtime)
static SystemContext& getInstance() {
    static SystemContext s_instance;
    return s_instance;
}
```

**Guidelines**:
- Use for system-wide resources
- Prevent copying with deleted copy constructors/assignment operators
- Private constructors/destructors for static instances

### 1.2 Dependency Injection

**Usage**: Decouple modules from concrete implementations

**Examples**:
- `DSP_pipeline` receives `IHardwareDriver*` in constructor
- `SystemContext` holds references to all injected dependencies
- Mock implementations can be injected for testing

**Implementation**:
```cpp
DSP_pipeline(IHardwareDriver* hardware_driver)
    : hardware_driver_(hardware_driver) { }
```

**Benefits**:
- Hardware-independent logic
- Easy to test with mock implementations
- Flexible module swapping

### 1.3 Service Locator Pattern

**Usage**: Central access point for system services

**Implementation**:
```cpp
SystemContext& system = SystemContext::getInstance();
DSP_pipeline* audio = system.getDSPPipeline();
IHardwareDriver* hw = system.getHardwareDriver();
```

**Note**: Combined with dependency injection for flexibility

### 1.4 Factory Pattern

**Usage**: Create FreeRTOS tasks with consistent interface

**Pattern**:
```cpp
namespace Module {
    static bool startTask(int core_id, UBaseType_t priority, uint32_t stack_words);
    // Internally: creates instance → begin() → task loop
}
```

---

## 2. Task Management Architecture

### 2.1 Task Initialization Pattern

**Standard Pattern** (used consistently across all modules):

```cpp
// Header (.h)
namespace Module {
    static bool startTask(int core_id, UBaseType_t priority,
                         uint32_t stack_words, std::size_t queue_len = 64);
}

// Implementation (.cpp)
namespace Module {
    static Module s_instance;  // Singleton

    bool startTask(int core_id, UBaseType_t priority,
                   uint32_t stack_words, std::size_t queue_len) {
        s_instance.createQueue(queue_len);
        BaseType_t ok = xTaskCreatePinnedToCore(
            taskTrampoline,    // Static entry point
            "module_name",     // Task name
            stack_words,       // Stack size in words
            &s_instance,       // Instance pointer
            priority,          // Priority
            &task_handle_,     // Handle output
            core_id            // Core assignment
        );
        return ok == pdPASS;
    }

    static void taskTrampoline(void* arg) {
        auto* self = static_cast<Module*>(arg);
        if (!self->begin()) {
            // Handle init failure
            vTaskDelete(nullptr);
            return;
        }
        // Infinite loop calling process()
        for (;;) {
            self->process();
        }
    }
}
```

**Benefits**:
- Consistent interface across all modules
- Implicit singleton management
- Clear lifetime semantics

### 2.2 Core Allocation Strategy

Core assignments are configurable at build time via `Config.h`. The typical default is:

**Core 0 (Real-Time Audio)**:
- `DSP_pipeline` (highest priority)
- Deterministic timing; keep I/O off this core

**Core 1 (I/O & Services)**:
- `Log` (medium priority)
- `VUMeter` (low priority)
- `RDSAssembler` (low priority)
- Arduino loop (idle)

Config parameters controlling pinning and priorities:
```
// Core selection (0 or 1)
CONSOLE_CORE, VU_CORE, RDS_CORE, DSP_CORE
// Priorities and stacks
CONSOLE_PRIORITY, VU_PRIORITY, RDS_PRIORITY, DSP_PRIORITY
CONSOLE_STACK_WORDS, VU_STACK_WORDS, RDS_STACK_WORDS, DSP_STACK_WORDS
```

Runtime verification: at startup each module logs its actual core ("… running on Core X"). The status panel CPU metrics also reflect the real assignment.

---

## 3. Communication Patterns

### 3.1 Queue Semantics

**Log Queue** (Drop-on-Overflow):
```cpp
// If queue full, drop oldest message
bool enqueuef(Level level, const char* fmt, ...);
// Returns: true if enqueued, false if dropped
```
- **Rationale**: Real-time systems must not block on logging
- **Behavior**: Messages may be lost under heavy load
- **Visibility**: Drop counter tracks messages lost

**VUMeter Queue** (Mailbox Overwrite):
```cpp
// If queue full, overwrite old sample with new one
bool enqueue(const Sample& s);
// Always succeeds - only latest sample matters
```
- **Rationale**: Only peak capture needed, not history
- **Behavior**: Always shows current state
- **Efficiency**: Minimal queue operations

**RDSAssembler Queue** (FIFO):
```cpp
// Ordered queue of RDS bits
xQueueCreate(1024, sizeof(uint8_t));
```
- **Rationale**: RDS bits must be sequential
- **Behavior**: Preserves bit order strictly

### 3.2 Error Handling Strategy

**Pattern**:
```cpp
// Hardware driver returns bool
if (!hardware_driver_->read(buffer, len, bytes_read)) {
    int err = hardware_driver_->getErrorStatus();
    Console::enqueuef(LogLevel::ERROR, "Read error: %d", err);
    ++stats_.errors;
    return;  // Skip this cycle
}
```

**Guidelines**:
- Return `bool` for success/failure
- Use `getErrorStatus()` for detailed error codes
- Log errors via Log module (non-blocking)
- Skip block or retry gracefully on error
- Never throw exceptions (embedded system)

---

## 4. Configuration Management

### 4.1 Centralized Configuration

**File**: `Config.h`

**Structure**:
```cpp
namespace Config {
    // GPIO pins
    constexpr int PIN_MCLK = 8;

    // Sample rates
    constexpr uint32_t SAMPLE_RATE_ADC = 48000;

    // Filter parameters
    constexpr float PREEMPHASIS_ALPHA = 0.6592f;

    // Feature toggles
    constexpr bool ENABLE_AUDIO = true;

    // Timing parameters
    constexpr uint64_t STATS_PRINT_INTERVAL_US = 5000000ULL;
}
```

**Guidelines**:
- All tunable parameters in Config.h
- Use `constexpr` for compile-time constants
- Document units and valid ranges
- Group by category
- No magic numbers in code

### 4.2 Configuration Sections

- GPIO Pin Assignments
- Sample Rates and Timing
- DSP Algorithm Parameters
- FM Stereo MPX Levels
- Feature Toggles
- Diagnostic Options
- Performance Monitoring
- VU Meter Settings
- Display Configuration

---

## 5. Hardware Abstraction

### 5.1 Interface Pattern

**File**: `IHardwareDriver.h`

```cpp
class IHardwareDriver {
public:
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool read(int32_t* buffer, std::size_t len,
                     std::size_t& bytes_read, uint32_t timeout) = 0;
    virtual bool write(const int32_t* buffer, std::size_t len,
                      std::size_t& bytes_written, uint32_t timeout) = 0;
    virtual uint32_t getInputSampleRate() const = 0;
    virtual uint32_t getOutputSampleRate() const = 0;
    virtual bool isReady() const = 0;
    virtual int getErrorStatus() const = 0;
    virtual bool reset() = 0;
};
```

**Benefits**:
- Hardware-independent business logic
- Easy to mock for unit testing
- Support multiple hardware backends
- Zero runtime overhead (virtual calls inlined)

### 5.2 Implementation

**File**: `ESP32I2SDriver.h/cpp`

Concrete implementation for ESP32-S3 with:
- I2S TX (192 kHz DAC output)
- I2S RX (48 kHz ADC input)
- Master clock coordination
- Error checking and status reporting

---

## 6. Code Organization

### 6.1 File Structure

**Headers (.h)**:
- Include guards (`#pragma once`)
- Forward declarations
- Class/namespace definitions
- Comprehensive documentation comments
- No implementation code

**Implementation (.cpp)**:
- Required includes
- Static helper functions (in anonymous namespace)
- Class method implementations
- Internal state management

### 6.2 Section Organization

Within files, organize into sections:
```cpp
// ==================================================================================
//                          SECTION NAME
// ==================================================================================
```

**Common Sections**:
- PUBLIC INTERFACE
- INITIALIZATION
- MAIN PROCESSING LOOP
- INTERNAL HELPERS
- DSP PIPELINE MODULES
- AUDIO BUFFERS
- FREERTOS TASK MANAGEMENT
- PERFORMANCE LOGGING
- DIAGNOSTIC LOGGING

---

## 7. Naming Conventions

### 7.1 Variables

**Member Variables**:
```cpp
float member_var_;           // Private member with trailing underscore
static float s_static_var;   // Static with s_ prefix
uint32_t param;              // Public parameter (no prefix)
```

**Local Variables**:
```cpp
size_t local_var;            // snake_case
float input_peak;            // Descriptive names
```

### 7.2 Functions

**Methods**:
```cpp
void process();              // camelCase for public methods
void printPerformance();     // Descriptive names
```

**Static Helpers**:
```cpp
static void taskTrampoline(void *arg);
```

**Namespaces**:
```cpp
namespace Log { ... }        // PascalCase
namespace Config { ... }
```

### 7.3 Constants

```cpp
constexpr int PIN_MCLK = 8;           // UPPER_SNAKE_CASE
constexpr float PREEMPHASIS_ALPHA = 0.6592f;
static const size_t kBufferSize = 256;  // k prefix for static constants
```

---

## 8. Documentation Standards

### 8.1 File Headers

Every source file starts with:
```cpp
/*
 * =====================================================================================
 *
 *                          PROJECT NAME
 *                        SHORT DESCRIPTION
 *
 * =====================================================================================
 *
 * File:         filename.h
 * Description:  One-line description
 *
 * Details...
 *
 * =====================================================================================
 */
```

### 8.2 Function Documentation

```cpp
/**
 * Function Brief Description
 *
 * Detailed explanation of what the function does, why, and how.
 * Include algorithm details if non-obvious.
 *
 * Parameters:
 *   param1:  Description and valid range
 *   param2:  Description and units
 *
 * Returns:
 *   return_type - Description of return value and semantics
 *
 * Note: Any important caveats or usage restrictions
 */
```

### 8.3 Inline Comments

```cpp
// Single-line explanation for non-obvious code
int result = complex_calculation();  // Why this formula?
```

---

## 9. Error Handling

### 9.1 Principles

- No exceptions (embedded system)
- Return bool for success/failure
- Use error codes for detailed diagnostics
- Log all errors
- Fail gracefully (skip block, retry, or shutdown)

### 9.2 Pattern

```cpp
if (!operation()) {
    int err = getErrorStatus();
    Console::enqueuef(LogLevel::ERROR, "Operation failed: %d", err);
    stats_.errors++;
    return;  // Or retry/recover
}
```

---

## 10. Performance Considerations

### 10.1 Real-Time Constraints

**DSP Pipeline**:
- Block size: 64 samples @ 48 kHz = 1.33 ms
- Target CPU: < 30% (70% headroom)
- No dynamic allocation in `process()`
- All buffers pre-allocated

### 10.2 Optimization Guidelines

- Avoid malloc/free in real-time paths
- Use stack allocation for small objects
- Align buffers to 16 bytes (SIMD potential)
- Profile before optimizing
- Document performance characteristics

---

## 11. Testing Strategy

### 11.1 Unit Testing

**Mock Hardware Driver**:
```cpp
class MockHardwareDriver : public IHardwareDriver {
    // Implement interface with test data
};
```

**Benefits**:
- Test DSP logic without hardware
- Deterministic test inputs
- No I2S dependencies

### 11.2 Integration Testing

- Full system with real hardware driver
- Audio quality verification
- Performance measurement

---

## 12. Future Enhancements

### 12.1 Planned

- [ ] SystemContext stub implementation
- [ ] Unit test framework setup
- [ ] Mock driver implementation
- [ ] Automated style checking

### 12.2 Possible

- [ ] Network audio driver
- [ ] USB audio support
- [ ] Configuration persistence
- [ ] OTA firmware updates

---

## References

- [FreeRTOS Task Creation](https://www.freertos.org/xTaskCreatePinnedToCore.html)
- [C++ Design Patterns](https://en.wikipedia.org/wiki/Software_design_pattern)
- [FM Stereo Encoding Standard](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.450-4-201910-I%21%21PDF-E.pdf)
- [Embedded C++ Guidelines](https://www.embedded.com/design/real-time-and-rtos/)

---

**Last Updated**: 2025-10-17
**Version**: 1.0
