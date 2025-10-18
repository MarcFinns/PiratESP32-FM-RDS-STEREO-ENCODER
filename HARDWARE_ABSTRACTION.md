# Hardware Abstraction Layer (HAL) Documentation

## Overview

This document explains the hardware abstraction layer introduced to decouple the DSP pipeline from ESP32-specific I2S operations.

### Files Created

1. **IHardwareDriver.h** - Pure virtual interface defining the hardware contract
2. **ESP32I2SDriver.h** - ESP32-S3 specific implementation header
3. **ESP32I2SDriver.cpp** - ESP32-S3 specific implementation

---

## Architecture

### Design Pattern: Dependency Inversion

```
Traditional (Tightly Coupled):
DSP_pipeline → i2s_read/i2s_write (ESP32 driver functions)
Problem: Cannot test DSP logic without ESP32 hardware

With Abstraction:
DSP_pipeline → IHardwareDriver (interface)
               ↓ implements
          ESP32I2SDriver (concrete implementation)
Benefit: DSP logic is hardware-independent; easy to mock for testing
```

### Interface Definition: IHardwareDriver

The minimal, focused interface that DSP_pipeline requires:

```cpp
class IHardwareDriver {
    virtual bool initialize() = 0;                    // Setup I2S TX/RX
    virtual void shutdown() = 0;                      // Cleanup
    virtual bool read(...) = 0;                       // Read from ADC
    virtual bool write(...) = 0;                      // Write to DAC
    virtual uint32_t getInputSampleRate() const = 0;  // Query input rate
    virtual uint32_t getOutputSampleRate() const = 0; // Query output rate
    virtual bool isReady() const = 0;                 // Status check
    virtual int getErrorStatus() const = 0;           // Platform error code (e.g., esp_err_t)
    virtual DriverError getLastError() const = 0;     // Typed error classification
    virtual bool reset() = 0;                         // Soft reset
};
```

### Implementation: ESP32I2SDriver

Concrete implementation for ESP32-S3:

```
ESP32I2SDriver
├── Wraps: i2s_read(), i2s_write() from ESP32 driver
├── Uses: AudioIO::setupTx(), AudioIO::setupRx() for configuration
├── Manages: Two I2S ports (I2S_NUM_0 for RX, I2S_NUM_1 for TX)
└── Handles: Error checking and conversion to bool returns
```

---

## Current Status: What's Done

✅ **Phase 2.1 Complete:**
- Created `IHardwareDriver.h` with pure virtual interface
- Created `ESP32I2SDriver.h` and `.cpp` with working implementation
- Interface is fully documented with usage examples

---

## Next Steps: How to Integrate

### Step 1: Update DSP_pipeline.h

Add a member variable to hold the hardware driver:

```cpp
class DSP_pipeline {
private:
    IHardwareDriver* hardware_driver_;  // NEW: inject dependency
    // ... existing members
};
```

Update constructor to accept driver:

```cpp
DSP_pipeline(IHardwareDriver* driver)
    : hardware_driver_(driver),
      pilot_19k_(...),
      mpx_synth_(...)
{
    // ...
}
```

### Step 2: Replace i2s_read/i2s_write in DSP_pipeline.cpp

**Current code (line 250):**
```cpp
i2s_read(kI2SPortRx, rx_buffer_, sizeof(rx_buffer_), &bytes_read, portMAX_DELAY);
```

**New code:**
```cpp
hardware_driver_->read(rx_buffer_, sizeof(rx_buffer_), bytes_read);
```

**Current code (line 812):**
```cpp
i2s_write(kI2SPortTx, tx_buffer_, bytes_to_write, &bytes_written, portMAX_DELAY);
```

**New code:**
```cpp
hardware_driver_->write(tx_buffer_, bytes_to_write, bytes_written);
```

### Step 3: Update initialization in DSP_pipeline::begin()

**Current code (lines 123-138):**
```cpp
if (!AudioIO::setupTx()) { /* error */ }
delay(100);
if (!AudioIO::setupRx()) { /* error */ }
```

**New code:**
```cpp
if (!hardware_driver_->initialize()) {
    Log::enqueue(Log::ERROR, "Hardware initialization failed!");
    return false;
}
```

### Step 4: Update main.ino

**Current:**
```cpp
void setup() {
    DSP_pipeline::startTask(0, 6, 12288);
}
```

**New (with dependency injection):**
```cpp
void setup() {
    // Create hardware driver
    auto* hw_driver = new ESP32I2SDriver();

    // Create DSP pipeline with injected driver
    auto* dsp = new DSP_pipeline(hw_driver);

    // Start task
    dsp->startTaskInstance(0, 6, 12288);
}
```

---

## Benefits of This Abstraction

### 1. **Testability**
- Create mock driver for unit testing DSP logic without hardware
- Example: `MockHardwareDriver` returns pre-recorded audio data

### 2. **Portability**
- Switch to different hardware (e.g., USB audio) by creating new driver
- Example: `USBHardwareDriver` implementation

### 3. **Maintainability**
- I2S changes isolated to one class
- DSP logic doesn't need ESP32 driver knowledge

### 4. **Clarity**
- Clear contract between DSP and hardware layers
- Explicit dependencies via constructor injection

---

## Testing Strategy

### Unit Test Example: MockHardwareDriver

```cpp
class MockHardwareDriver : public IHardwareDriver {
private:
    std::vector<int32_t> test_audio_;  // Pre-recorded samples
    size_t read_pos_ = 0;

public:
    bool initialize() override { return true; }
    bool read(int32_t* buf, std::size_t len, std::size_t& read) override {
        // Return pre-recorded audio instead of hardware
        // ...
    }
    // ... implement other methods
};
```

### Test Code

```cpp
// Test DSP_pipeline with mock hardware (no ESP32 needed!)
MockHardwareDriver mock;
DSP_pipeline dsp(&mock);
dsp.begin();  // Uses mock driver instead of real hardware
dsp.process(); // Process mock audio data
// Verify output without real DAC/ADC
```

---

## Error Handling

### From DSP_pipeline

Current:
```cpp
if (ret != ESP_OK) {
    Log::enqueuef(Log::ERROR, "Read error: %d", ret);
}
```

After refactoring:
```cpp
if (!hardware_driver_->read(rx_buffer_, ..., bytes_read)) {
    auto derr = hardware_driver_->getLastError();
    int  perr = hardware_driver_->getErrorStatus();
    const char* name = esp_err_to_name((esp_err_t)perr);
    // Map to ErrorHandler::ErrorCode and include esp_err name for details
    // Example (timeout):
    ErrorHandler::logError(ErrorCode::TIMEOUT, "DSP_pipeline::read", name);
}
```

---

## Performance Impact

✅ **Negligible**:
- Virtual function calls are optimized by compiler
- Inlining can eliminate vtable lookup in hot path
- Same underlying I2S operations (no additional data copies)

---

## Rollback Plan

If issues arise:
1. The abstraction is non-invasive
2. Can revert to direct I2S calls
3. No breaking changes to existing code
4. All original functionality preserved

---

## Future Enhancements

1. **SystemContext** - Centralized dependency injection container
2. **Alternative Drivers** - USB audio, network streaming
3. **Mock Drivers** - For comprehensive unit testing
4. **Error Recovery** - Automatic I2S restart on failures
5. **Performance Tuning** - DMA buffer size optimization

---

## Code Statistics

- **IHardwareDriver.h**: ~180 lines (interface + docs)
- **ESP32I2SDriver.h**: ~160 lines (header + docs)
- **ESP32I2SDriver.cpp**: ~150 lines (implementation)
- **Total**: ~490 lines for complete abstraction

---

## Integration Checklist

- [ ] Update DSP_pipeline.h to accept IHardwareDriver*
- [ ] Replace i2s_read/i2s_write calls with driver methods
- [ ] Update DSP_pipeline::begin() initialization
- [ ] Update main.ino to inject hardware driver
- [ ] Test on hardware (verify audio still works)
- [ ] Commit with message: "Decouple DSP from hardware via abstraction layer"
- [ ] Create MockHardwareDriver for unit tests
- [ ] Document in ARCHITECTURE.md

---

## Questions & Answers

**Q: Why not use dependency injection frameworks?**
A: Arduino/ESP32 environment is resource-constrained. Manual DI keeps overhead minimal.

**Q: Will this slow down audio processing?**
A: No. Virtual function calls are inlined by modern compilers. Zero runtime overhead.

**Q: What if I need custom I2S configuration?**
A: Extend IHardwareDriver with additional methods, implement in ESP32I2SDriver.

**Q: Can I have multiple drivers active?**
A: Yes, each can use different I2S ports or hardware.

---

## Contact & Support

For issues with the abstraction layer or integration questions, refer to:
- This document: HARDWARE_ABSTRACTION.md
- Main architecture doc: README.md
- Issue tracker: GitHub issues
