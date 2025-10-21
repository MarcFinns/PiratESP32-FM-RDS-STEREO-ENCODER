/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *             Serial Console: SCPI-style Command Parser + Log Drainer
 *
 * =====================================================================================
 *
 * File:         Console.cpp
 * Description:  Dual-purpose module providing:
 *               1. SCPI-style command parser for device configuration & monitoring
 *               2. Non-blocking log draining to prevent Serial I/O blocking audio
 *
 * Purpose:
 *   Provides a complete serial console interface that:
 *   - Parses SCPI-style commands (GROUP:ITEM <value> or GROUP:ITEM?)
 *   - Manages configuration (RDS, audio, pilot, system parameters)
 *   - Supports both text and JSON response formats
 *   - Handles non-blocking logging from real-time audio processing
 *   - Persists configuration to NVS (non-volatile storage)
 *   - Never blocks the audio processing thread
 *
 * Command Parser Architecture:
 *   • Input: Serial line "GROUP:ITEM <args>" or "GROUP:ITEM?"
 *   • Tokenization: Extracts GROUP, ITEM, and remaining arguments
 *   • Dispatch: Routes to RDS, AUDIO, PILOT, or SYST command handlers
 *   • Validation: Type checking and range validation for each parameter
 *   • Response: Text "OK key=value" or JSON {"ok":true,"data":{...}}
 *   • Error: Text "ERR code message" or JSON {"ok":false,"error":{...}}
 *
 * Design Principles:
 *   • Zero blocking: Serial enqueue never waits, message drop on overflow with counter
 *   • Fixed-size allocation: No dynamic memory in real-time logging path
 *   • Bounded latency: Message formatting in caller context, not logger
 *   • Core isolation: Serial I/O runs exclusively on Core 1
 *   • Case-insensitive: Commands work as "RDS:PI", "rds:pi", "Rds:Pi"
 *   • Whitespace-tolerant: "RDS:PI 0x123" and "RDS:PI  0x123" both work
 *   • ModuleBase compliance: Unified task lifecycle management
 *
 * Performance Characteristics:
 *   • Command parse+execute: <1ms for most commands
 *   • Enqueue time: ~5-10 µs (FreeRTOS queue overhead + copy)
 *   • Message size: 160 bytes per message (timestamp + level + 159-char string)
 *   • Queue depth: Configurable (default 64 messages = 10 KB)
 *   • Drop behavior: Silent drop with atomic counter increment
 *   • JSON response: Single-line JSON for scripting compatibility
 *
 * Thread Safety:
 *   All public functions are safe to call from any task or ISR.
 *   FreeRTOS queues provide atomic operations with lockless semantics.
 *   Static parser state is protected by FreeRTOS task isolation (Core 1 only).
 *
 * Configuration Persistence:
 *   Settings are saved to ESP32 NVS (Preferences) by SYST:CONF:SAVE
 *   Last active configuration is restored on boot via Console_LoadLastConfiguration()
 *   Configuration includes RDS, audio, pilot, and system settings
 *
 * =====================================================================================
 */

#include "Console.h"
#include "Config.h"
#include "DisplayManager.h"
#include "RDSAssembler.h"

#include "DSP_pipeline.h"
#include "DisplayManager.h"
#include "RDSAssembler.h"
#include "TaskStats.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstdarg>
#include <cstddef>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ==================================================================================
//                      GLOBAL STATE FOR LOGGING CONTROL
// ==================================================================================

static LogLevel s_min_level = LogLevel::DEBUG;
static bool s_log_mute = false; // when true, suppress background log prints
static bool s_json_mode = false;
static bool s_startup_phase =
    true; // Remains true through initial boot, set to false after "System Ready"
static bool s_mute_after_startup =
    false; // Set to true if config says LOG_LEVEL=255, applied when startup ends

// ==================================================================================
//                          LOG MESSAGE STRUCTURE
// ==================================================================================

/**
 * Internal Log Message Structure
 *
 * Fixed-size structure stored in the FreeRTOS queue. Each message contains:
 *   • Timestamp: Microsecond timestamp from micros() at enqueue time
 *   • Level: Severity level (DEBUG, INFO, WARN, ERROR)
 *   • Text: Null-terminated string (max 159 chars + null)
 *
 * Size: 160 bytes total (4 + 1 + 3 padding + 160 = 168 bytes aligned)
 */
struct LogMsg
{
    uint32_t ts_us; // Timestamp in microseconds (from micros())
    uint8_t level;  // Log level (DEBUG=0, INFO=1, WARN=2, ERROR=3)
    char text[160]; // Message text (null-terminated, max 159 chars)
};

// ==================================================================================
//                          SINGLETON INSTANCE
// ==================================================================================

/**
 * Get Console Singleton Instance
 *
 * Returns the single global Console instance using Meyer's singleton pattern.
 * Thread-safe and lazy-initialized.
 *
 * Returns:
 *   Reference to the singleton Console instance
 */
Console &Console::getInstance()
{
    static Console s_instance;
    return s_instance;
}

// ==================================================================================
//                          CONSTRUCTOR & MEMBER INITIALIZATION
// ==================================================================================

/**
 * Private Constructor (Singleton Pattern)
 *
 * Initializes all member variables to safe default states.
 * Cannot be called directly - use getInstance() instead.
 */
Console::Console()
    : queue_(nullptr), queue_len_(64), dropped_count_(0), core_id_(1), priority_(2),
      stack_words_(4096)
{
    // All members initialized via initializer list
}

// ==================================================================================
//                          STATIC WRAPPER API
// ==================================================================================

/**
 * Static Wrapper - Initialize Console and Start Task
 *
 * Delegates to the singleton instance.
 */
bool Console::begin(size_t queue_len, int core_id, uint32_t priority, uint32_t stack_words)
{
    Console &logger = getInstance();
    logger.queue_len_ = queue_len;
    logger.core_id_ = core_id;
    logger.priority_ = priority;
    logger.stack_words_ = stack_words;

    // Spawn the console task via ModuleBase helper
    return logger.spawnTask("console", (uint32_t)stack_words, (UBaseType_t)priority, core_id,
                            Console::taskTrampoline);
}

/**
 * Static Wrapper - Start Console Task (Convenience)
 *
 * Alternative parameter order for consistency with other modules.
 */
bool Console::startTask(int core_id, uint32_t priority, uint32_t stack_words, size_t queue_len)
{
    return begin(queue_len, core_id, priority, stack_words);
}

/**
 * Static Wrapper - Enqueue Formatted Message
 *
 * Delegates to the singleton instance.
 */
bool Console::enqueuef(LogLevel level, const char *fmt, ...)
{
    if (!fmt)
        return false;

    va_list ap;
    va_start(ap, fmt);
    bool result = getInstance().enqueueFormatted(level, fmt, ap);
    va_end(ap);

    return result;
}

/**
 * Static Wrapper - Enqueue Preformatted Message
 *
 * Delegates to the singleton instance.
 */
bool Console::enqueue(LogLevel level, const char *msg)
{
    return getInstance().enqueueRaw(level, msg);
}

bool Console::printOrSerial(LogLevel level, const char *msg)
{
    Console &inst = getInstance();
    if (inst.queue_ && msg)
    {
        return inst.enqueueRaw(level, msg);
    }
    if (msg)
    {
        const char *lvl = (level == LogLevel::ERROR)  ? "ERROR"
                          : (level == LogLevel::WARN) ? "WARN"
                          : (level == LogLevel::INFO) ? "INFO"
                                                      : "DEBUG";
        Serial.printf("[%s] %s\n", lvl, msg);
    }
    return false;
}

bool Console::printfOrSerial(LogLevel level, const char *fmt, ...)
{
    if (!fmt)
        return false;

    Console &inst = getInstance();

    if (inst.queue_)
    {
        va_list ap;
        va_start(ap, fmt);
        bool ok = inst.enqueueFormatted(level, fmt, ap);
        va_end(ap);
        return ok;
    }

    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *lvl = (level == LogLevel::ERROR)  ? "ERROR"
                      : (level == LogLevel::WARN) ? "WARN"
                      : (level == LogLevel::INFO) ? "INFO"
                                                  : "DEBUG";
    Serial.printf("[%s] %s\n", lvl, buf);
    return false;
}

bool Console::isReady()
{
    return getInstance().isRunning();
}

void Console::markStartupComplete()
{
    // Log startup completion message before applying mute
    if (s_mute_after_startup)
    {
        Console::enqueuef(LogLevel::INFO, "Startup complete - periodic logging will now be muted");
    }
    else
    {
        Console::enqueuef(LogLevel::INFO, "Startup complete - continuing with full logging");
    }

    s_startup_phase = false;
    // Apply deferred mute state if configuration had LOG_LEVEL=255
    if (s_mute_after_startup)
    {
        s_log_mute = true;
    }
}

// ==================================================================================
//                          MODULEBASE IMPLEMENTATION
// ==================================================================================

/**
 * Task Trampoline (FreeRTOS Entry Point)
 *
 * Static function called by FreeRTOS when task starts.
 * Extracts the Log instance pointer and calls defaultTaskTrampoline().
 */
void Console::taskTrampoline(void *arg)
{
    ModuleBase::defaultTaskTrampoline(arg);
}

/**
 * Initialize Module Resources (ModuleBase contract)
 *
 * Called once when the task starts. Creates the logger queue and initializes
 * Serial communication.
 */
bool Console::begin()
{
    // ---- Initialize Serial if Not Already Started ----
    if (!Serial)
    {
        Serial.begin(115200); // Standard baud rate for ESP32
        delay(50);            // Allow Serial to stabilize
    }

    // ---- Create FreeRTOS Queue ----
    queue_ = xQueueCreate((UBaseType_t)queue_len_, sizeof(LogMsg));
    if (queue_ == nullptr)
    {
        // Queue creation failed (likely out of heap memory)
        return false;
    }

    // Print runtime pinning info via Serial to avoid recursion during console init
    Serial.printf("Console running on Core %d\n", xPortGetCoreID());

    // ---- Emit startup banner via logger queue ----
    // These messages will be printed once the process loop starts
    enqueueRaw(LogLevel::INFO, "========================================");
    enqueueRaw(LogLevel::INFO, "PiratESP32 FM RDS STEREO ENCODER");
    enqueueRaw(LogLevel::INFO, "Copyright (c) 2024-2025 PiratESP32 contributors");
    {
        char build[96];
        snprintf(build, sizeof(build), "Build: %s %s", __DATE__, __TIME__);
        enqueueRaw(LogLevel::INFO, build);
    }
    enqueueRaw(LogLevel::INFO, "========================================");

    return true;
}

/**
 * Main Processing Loop Body (ModuleBase contract)
 *
 * Called repeatedly in infinite loop. Drains one message from the queue
 * and outputs it to Serial. If queue is empty, blocks waiting for message.
 */

// -------------------- Preferences (NVS) for SYST:CONF:* --------------------
static Preferences s_prefs;
static const char *kPrefsNs = "conf";
static bool s_prefs_open = false;

static void conf_open_rw()
{
    if (!s_prefs_open)
    {
        s_prefs.begin(kPrefsNs, false);
        s_prefs_open = true;
    }
}
static void conf_close()
{
    if (s_prefs_open)
    {
        s_prefs.end();
        s_prefs_open = false;
    }
}

static bool strlist_contains(const String &csv, const char *name)
{
    int start = 0;
    while (start >= 0)
    {
        int comma = csv.indexOf(',', start);
        String token = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        token.trim();
        if (token.length() > 0 && token.equals(String(name)))
            return true;
        if (comma < 0)
            break;
        else
            start = comma + 1;
    }
    return false;
}

static String strlist_add_unique(const String &csv, const char *name)
{
    if (strlist_contains(csv, name))
        return csv;
    if (csv.length() == 0)
        return String(name);
    return csv + "," + name;
}

static String strlist_remove(const String &csv, const char *name)
{
    String out;
    int start = 0;
    while (start >= 0)
    {
        int comma = csv.indexOf(',', start);
        String token = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        token.trim();
        if (token.length() > 0 && token != String(name))
        {
            if (out.length() > 0)
                out += ",";
            out += token;
        }
        if (comma < 0)
            break;
        else
            start = comma + 1;
    }
    return out;
}

// ==================================================================================
//                         HELPER FUNCTIONS
// ==================================================================================

/**
 * Trim trailing spaces from RDS strings
 *
 * RDS spec requires PS to be exactly 8 chars and RT to be exactly 64 chars,
 * so they are padded with spaces. When displaying these strings to users,
 * we want to show only the meaningful content without trailing spaces.
 *
 * Modifies the buffer in-place by replacing trailing spaces with null terminator.
 */
static void trim_trailing_spaces(char *str)
{
    if (!str)
        return;
    int len = strlen(str);
    while (len > 0 && str[len - 1] == ' ')
    {
        str[len - 1] = '\0';
        len--;
    }
}

static void conf_build_blob(char *buf, size_t sz)
{
    char ps[9] = {0};
    RDSAssembler::getPS(ps);
    trim_trailing_spaces(ps);
    char rt[65] = {0};
    RDSAssembler::getRT(rt);
    trim_trailing_spaces(rt);
    // Build RT list as quoted pipe-separated
    char rtlist[512];
    rtlist[0] = '\0';
    bool first = true;
    for (std::size_t i = 0; i < RDSAssembler::rtListCount(); ++i)
    {
        char t[160];
        if (RDSAssembler::rtListGet(i, t, sizeof(t)))
        {
            char part[192];
            snprintf(part, sizeof(part), "%s\"%s\"", first ? "" : "|", t);
            strncat(rtlist, part, sizeof(rtlist) - strlen(rtlist) - 1);
            first = false;
        }
    }
    snprintf(buf, sz,
             "PI=0x%04X;PTY=%u;TP=%u;TA=%u;MS=%u;PS=\"%s\";RT=\"%s\";RTPERIOD=%u;RTLIST=%s;"
             "AUDIO_STEREO=%u;PREEMPH=%u;RDS_ENABLE=%u;PILOT_ENABLE=%u;PILOT_AUTO=%u;PILOT_THRESH=%"
             "g;PILOT_HOLD=%u;LOG_LEVEL=%u",
             (unsigned)RDSAssembler::getPI(), (unsigned)RDSAssembler::getPTY(),
             (unsigned)RDSAssembler::getTP(), (unsigned)RDSAssembler::getTA(),
             (unsigned)RDSAssembler::getMS(), ps, rt, (unsigned)RDSAssembler::getRtPeriod(), rtlist,
             DSP_pipeline::getStereoEnable() ? 1 : 0, DSP_pipeline::getPreemphEnable() ? 1 : 0,
             DSP_pipeline::getRdsEnable() ? 1 : 0, DSP_pipeline::getPilotEnable() ? 1 : 0,
             DSP_pipeline::getPilotAuto() ? 1 : 0, (double)DSP_pipeline::getPilotThresh(),
             (unsigned)DSP_pipeline::getPilotHold(), s_log_mute ? 255 : (unsigned)s_min_level);
}

static const char *find_key(const char *blob, const char *key)
{
    const char *p = strstr(blob, key);
    if (!p)
        return nullptr;
    p += strlen(key);
    if (*p == '=')
        ++p; // allow key=value or keyvalue
    return p;
}

static void apply_loaded_blob(const char *blob)
{
    if (!blob)
        return;
    // Simple key scanning
    auto read_int = [&](const char *k, int def) -> int
    {
        const char *p = find_key(blob, k);
        if (!p)
            return def;
        return (strncmp(p, "0x", 2) == 0 || strncmp(p, "0X", 2) == 0)
                   ? (int)strtoul(p, nullptr, 16)
                   : (int)strtoul(p, nullptr, 10);
    };
    auto read_uint = [&](const char *k, unsigned def) -> unsigned
    {
        const char *p = find_key(blob, k);
        if (!p)
            return def;
        return (unsigned)strtoul(p, nullptr, 10);
    };
    auto read_float = [&](const char *k, float def) -> float
    {
        const char *p = find_key(blob, k);
        if (!p)
            return def;
        return (float)atof(p);
    };
    auto read_str = [&](const char *k, char *out, size_t outsz)
    {
        const char *p = find_key(blob, k);
        if (!p)
            return;
        if (*p == '\"')
        {
            ++p;
            size_t i = 0;
            while (*p && *p != '\"' && i < outsz - 1)
            {
                out[i++] = *p++;
            }
            out[i] = '\0';
        }
    };

    int pi = read_int("PI", RDSAssembler::getPI());
    int pty = read_int("PTY", RDSAssembler::getPTY());
    int tp = read_int("TP", RDSAssembler::getTP() ? 1 : 0);
    int ta = read_int("TA", RDSAssembler::getTA() ? 1 : 0);
    int ms = read_int("MS", RDSAssembler::getMS() ? 1 : 0);
    char ps[64] = {0};
    read_str("PS", ps, sizeof(ps));
    char rt[128] = {0};
    read_str("RT", rt, sizeof(rt));
    unsigned rtper = read_uint("RTPERIOD", RDSAssembler::getRtPeriod());
    // Apply
    RDSAssembler::setPI((uint16_t)(pi & 0xFFFF));
    RDSAssembler::setPTY((uint8_t)(pty & 0x1F));
    RDSAssembler::setTP(tp != 0);
    RDSAssembler::setTA(ta != 0);
    RDSAssembler::setMS(ms != 0);
    if (ps[0])
        RDSAssembler::setPS(ps);
    if (rt[0])
        RDSAssembler::setRT(rt);
    RDSAssembler::setRtPeriod(rtper);
    // RTLIST: RTLIST="a"|"b"|"c"
    const char *pr = find_key(blob, "RTLIST");
    if (pr)
    {
        RDSAssembler::rtListClear();
        while (*pr)
        {
            while (*pr && *pr != '\"' && *pr != ';')
                ++pr;
            if (*pr == '\"')
            {
                ++pr;
                char item[256];
                size_t i = 0;
                while (*pr && *pr != '\"' && i < sizeof(item) - 1)
                {
                    item[i++] = *pr++;
                }
                item[i] = '\0';
                RDSAssembler::rtListAdd(item);
                while (*pr && *pr != '|' && *pr != ';')
                    ++pr;
                if (*pr == '|')
                    ++pr;
                else
                    break;
            }
            else
                break;
        }
    }
    // Audio & pilot & RDS toggles
    DSP_pipeline::setStereoEnable(
        read_int("AUDIO_STEREO", DSP_pipeline::getStereoEnable() ? 1 : 0) != 0);
    DSP_pipeline::setPreemphEnable(read_int("PREEMPH", DSP_pipeline::getPreemphEnable() ? 1 : 0) !=
                                   0);
    DSP_pipeline::setRdsEnable(read_int("RDS_ENABLE", DSP_pipeline::getRdsEnable() ? 1 : 0) != 0);
    DSP_pipeline::setPilotEnable(read_int("PILOT_ENABLE", DSP_pipeline::getPilotEnable() ? 1 : 0) !=
                                 0);
    DSP_pipeline::setPilotAuto(read_int("PILOT_AUTO", DSP_pipeline::getPilotAuto() ? 1 : 0) != 0);
    DSP_pipeline::setPilotThresh(read_float("PILOT_THRESH", DSP_pipeline::getPilotThresh()));
    DSP_pipeline::setPilotHold(read_uint("PILOT_HOLD", DSP_pipeline::getPilotHold()));

    // Load log level setting (if present in configuration)
    // Default to DEBUG if not specified
    // Special value 255 means "OFF" (muted) - but we defer enabling mute until after startup
    unsigned log_level = read_uint("LOG_LEVEL", (unsigned)LogLevel::DEBUG);
    if (log_level == 255)
    {
        // Mute flag will be set AFTER startup phase completes (in markStartupComplete)
        // This allows the full startup sequence to be logged even if mute was saved
        s_mute_after_startup = true;
        s_log_mute = false; // Keep active during startup
    }
    else if (log_level <= (unsigned)LogLevel::ERROR)
    {
        s_mute_after_startup = false;
        s_log_mute = false;
        s_min_level = static_cast<LogLevel>(log_level);
    }
}

// ==================================================================================
//                    FACTORY DEFAULT VALUES
// ==================================================================================

/**
 * Apply Factory Default Configuration
 *
 * Resets all settings to factory defaults:
 * - RDS: Default PI, PTY, TP/TA/MS flags
 * - PS: "PiratESP" (8 characters)
 * - RT: "Hello from ESP32 FM Stereo RDS encoder!" (up to 64 chars)
 * - Audio: Stereo enabled, pre-emphasis per config
 * - Pilot: Enabled, with auto-mute settings
 * - RDS subcarrier: Enabled
 */
static void apply_factory_defaults()
{
    // RDS core settings
    DSP_pipeline::setRdsEnable(Config::ENABLE_RDS_57K);
    DSP_pipeline::setStereoEnable(Config::ENABLE_STEREO_SUBCARRIER_38K);
    DSP_pipeline::setPreemphEnable(Config::ENABLE_PREEMPHASIS);
    DSP_pipeline::setPilotEnable(Config::ENABLE_STEREO_PILOT_19K);
    DSP_pipeline::setPilotAuto(Config::PILOT_MUTE_ON_SILENCE);
    DSP_pipeline::setPilotThresh(Config::SILENCE_RMS_THRESHOLD);
    DSP_pipeline::setPilotHold(Config::SILENCE_HOLD_MS);

    // RDS content defaults
    RDSAssembler::setPS("PiratESP");
    RDSAssembler::setRT("Hello from ESP32 FM Stereo RDS encoder!");
    RDSAssembler::rtListClear();
    RDSAssembler::setRtPeriod(30);

    // Console defaults
    // Set default log level to INFO (balance between information and verbosity)
    s_min_level = LogLevel::INFO;
    s_log_mute = false;
    s_mute_after_startup = false;
    s_startup_phase = true; // Reset to startup phase when applying defaults
}

// ==================================================================================
//                    CONFIGURATION LOADING AT BOOT
// ==================================================================================

/**
 * Load Last Configuration or Factory Defaults
 *
 * Called during system initialization (after RDS and DSP modules start).
 * Attempts to load the last active configuration from NVS. If no saved
 * configuration exists, applies factory defaults.
 *
 * This function:
 * 1. Opens preferences (NVS)
 * 2. Checks if an active configuration exists
 * 3. Loads it if present, otherwise applies factory defaults
 * 4. Closes preferences
 */
extern void Console_LoadLastConfiguration()
{
    Preferences prefs;
    if (!prefs.begin("conf", false))
    {
        // Failed to open preferences - apply factory defaults
        apply_factory_defaults();
        return;
    }

    String active = prefs.getString("_active", "");
    if (active.length() == 0)
    {
        // No active configuration saved - apply factory defaults
        prefs.end();
        apply_factory_defaults();
        return;
    }

    // Load the saved configuration
    String key = String("p:") + active;
    String blob = prefs.getString(key.c_str(), "");
    prefs.end();

    if (blob.length() > 0)
    {
        // Configuration found - apply it
        apply_loaded_blob(blob.c_str());
    }
    else
    {
        // Configuration name exists but no data - apply factory defaults
        apply_factory_defaults();
    }
}

void Console::process()
{
    // ==================================================================================
    // SECTION 1: LOG MESSAGE DRAINING
    // ==================================================================================
    // Drain queued log messages to Serial in a non-blocking manner.
    // Limited to MAX_LOGS_PER_LOOP messages per cycle to keep console responsive
    // and allow time for command processing. Logs are filtered by s_min_level
    // and can be muted entirely with s_log_mute flag.
    // ==================================================================================

    static constexpr int MAX_LOGS_PER_LOOP = 4;
    for (int i = 0; i < MAX_LOGS_PER_LOOP; ++i)
    {
        LogMsg msg;
        if (xQueueReceive(queue_, &msg, 0) == pdTRUE)
        {
            // Allow logs during startup phase, or when not muted and level meets threshold
            bool should_log =
                (s_startup_phase || !s_log_mute) && msg.level >= static_cast<uint8_t>(s_min_level);
            if (should_log)
                Serial.printf("[%8u] %s\n", (unsigned)msg.ts_us, msg.text);
        }
        else
        {
            break;
        }
    }

    // ==================================================================================
    // SECTION 2: COMMAND LINE RECEPTION & PARSING
    // ==================================================================================
    // Receive serial bytes and accumulate them into complete lines terminated by \n.
    // When a complete line is received, tokenize it and dispatch to appropriate handler.
    // Process one command per loop cycle to maintain responsiveness.
    // ==================================================================================

    // Note: haveLine() lambda is unused legacy code, kept for compatibility
    auto haveLine = []() -> bool
    {
        static char buf[256];
        static size_t len = 0;
        while (Serial.available() > 0)
        {
            int c = Serial.read();
            if (c < 0)
                break;
            char ch = (char)c;
            if (ch == '\r')
                continue;
            if (ch == '\n')
            {
                buf[len] = '\0';
                len = 0;
                // Store line in a static place for retrieval
                strncpy((char *)buf, (const char *)buf, sizeof(buf) - 1);
                // misuse return; we will fetch via getter below
                return true;
            }
            if (len < sizeof(buf) - 1)
                buf[len++] = ch;
        }
        return false;
    };

    static char line_buf[256];  // Input buffer for current command line
    static size_t line_len = 0; // Current length of line_buf

    // ---- STEP 1: READ SERIAL BYTES INTO line_buf ----
    // Read all available bytes from Serial, accumulate into line_buf until newline
    while (Serial.available() > 0)
    {
        int c = Serial.read();
        if (c < 0)
            break;
        char ch = (char)c;
        if (ch == '\r')
            continue;
        if (ch == '\n')
        {
            // ---- STEP 2: COMPLETE LINE RECEIVED ----
            // Null-terminate the line and prepare for tokenization/parsing
            line_buf[line_len] = '\0';
            line_len = 0; // Reset for next command

            // ---- STEP 3: DEFINE PARSING HELPER LAMBDAS ----
            // These helper functions perform common string parsing operations

            // trim(): Remove leading and trailing whitespace from a string
            // Used to clean up the input command line
            auto trim = [](char *s)
            {
                // left trim: skip leading spaces/tabs
                char *p = s;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (p != s)
                    memmove(s, p, strlen(p) + 1);

                // right trim: skip trailing spaces/tabs
                size_t n = strlen(s);
                while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
                    s[--n] = '\0';
            };

            // parse_quoted(): Extract a quoted string value with escape sequence support
            // Supports both quoted strings ("value") and unquoted strings (value)
            // Handles escape sequences: \" becomes ", \n becomes newline, etc.
            // Used for parsing RDS:PS and RDS:RT arguments
            auto parse_quoted = [](const char *&p, char *out, size_t outsz)
            {
                // skip leading whitespace
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (*p == '"')
                {
                    // Quoted string: parse until closing quote
                    ++p; // Skip opening quote
                    size_t i = 0;
                    while (*p && *p != '"' && i < outsz - 1)
                    {
                        if (*p == '\\' && *(p + 1) != 0)
                        {
                            // Handle escape sequences (e.g., \", \n)
                            ++p;
                            out[i++] = *p++;
                        }
                        else
                        {
                            out[i++] = *p++;
                        }
                    }
                    if (*p == '"')
                        ++p; // Skip closing quote
                    out[i] = '\0';
                    return true;
                }
                // Unquoted string: read until end
                size_t i = 0;
                while (*p && i < outsz - 1)
                {
                    out[i++] = *p++;
                }
                out[i] = '\0';
                return true;
            };

            // iequal(): Case-insensitive string comparison
            // Converts both strings to uppercase before comparing
            // Allows commands like "RDS:PI", "rds:pi", "Rds:Pi" to all work
            auto iequal = [](const char *a, const char *b)
            {
                while (*a && *b)
                {
                    // Convert to uppercase for comparison
                    char ca = (*a >= 'a' && *a <= 'z') ? (*a - 32) : *a;
                    char cb = (*b >= 'a' && *b <= 'z') ? (*b - 32) : *b;
                    if (ca != cb)
                        return false;
                    ++a;
                    ++b;
                }
                return *a == 0 && *b == 0; // Both must end at null terminator
            };

            // ---- STEP 4: TOKENIZE INPUT LINE ----
            // Copy line to working buffer and split into GROUP:ITEM and arguments

            static char line[256];
            strncpy(line, (const char *)line_buf, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            trim(line); // Remove leading/trailing whitespace

            // next_token(): Extract next space/colon-delimited token from string pointer
            // Advances pointer past the extracted token and any delimiters
            // Used to extract GROUP, ITEM from "GROUP:ITEM <args>"
            auto next_token = [](const char *&p, char *out, size_t outsz)
            {
                // Skip leading spaces, tabs, and colons (delimiters)
                while (*p == ' ' || *p == '\t' || *p == ':')
                    ++p;
                if (!*p)
                {
                    out[0] = '\0';
                    return;
                }
                // Copy token until next delimiter (space, tab, or colon)
                size_t i = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != ':')
                {
                    if (i < outsz - 1)
                        out[i++] = *p;
                    ++p;
                }
                out[i] = '\0';
            };

            // ---- EXTRACT COMMAND COMPONENTS ----
            // Command format: "GROUP:ITEM <args>" or "GROUP:ITEM?"
            // Example: "RDS:PI 0x52A1" -> group="RDS", item="PI", rest="0x52A1"
            const char *sp = line;
            char group_tok[32]; // GROUP part (e.g., "RDS")
            char item_tok[64];  // ITEM part (e.g., "PI?" or "PI")
            next_token(sp, group_tok, sizeof(group_tok));
            next_token(sp, item_tok, sizeof(item_tok));
            const char *rest = sp;

            // Trim any remaining leading whitespace from arguments
            // This is CRITICAL to prevent parsing bugs with whitespace
            while (*rest == ' ' || *rest == '\t' || *rest == ':')
                ++rest;

            bool handled = false;

            // ---- DEFINE RESPONSE HELPER LAMBDAS ----
            // These lambdas format and send responses in either text or JSON format

            // ok(): Send success response without data (e.g., set command)
            auto ok = [&]()
            {
                if (!s_json_mode)
                    Serial.println("OK");
                else
                    Serial.println("{\"ok\":true}");
            };

            // ok_kv(): Send success response with key=value data (e.g., get command)
            auto ok_kv = [&](const char *kv)
            {
                if (!s_json_mode)
                {
                    Serial.print("OK ");
                    Serial.println(kv ? kv : "");
                }
                else
                {
                    // Build a proper JSON object from a comma-separated key=value list
                    Serial.print("{\"ok\":true,\"data\":{");
                    const char *p = kv ? kv : "";
                    bool first = true;
                    auto print_escaped = [&](const char *s, const char *e)
                    {
                        while (s < e)
                        {
                            char c = *s++;
                            if (c == '\\' || c == '\"')
                            {
                                Serial.print('\\');
                                Serial.print(c);
                            }
                            else if ((unsigned char)c < 0x20)
                            {
                                char buf[8];
                                snprintf(buf, sizeof(buf), "\\u%04X", (unsigned)(unsigned char)c);
                                Serial.print(buf);
                            }
                            else
                            {
                                Serial.print(c);
                            }
                        }
                    };
                    while (*p)
                    {
                        // skip leading spaces
                        while (*p == ' ')
                            ++p;
                        const char *start = p;
                        bool in_quotes = false;
                        char prev = 0;
                        while (*p)
                        {
                            char c = *p;
                            if (c == '"' && prev != '\\')
                                in_quotes = !in_quotes;
                            if (c == ',' && !in_quotes)
                                break;
                            prev = c;
                            ++p;
                        }
                        const char *end = p; // [start,end)
                        // advance over comma for next token
                        if (*p == ',')
                            ++p;
                        // find '=' inside token (not inside quotes)
                        const char *eq = start;
                        in_quotes = false;
                        prev = 0;
                        while (eq < end)
                        {
                            char c = *eq;
                            if (c == '"' && prev != '\\')
                                in_quotes = !in_quotes;
                            if (c == '=' && !in_quotes)
                                break;
                            prev = c;
                            ++eq;
                        }
                        if (eq >= end)
                            continue; // no key=value pair, skip
                        // trim key
                        const char *k0 = start;
                        while (k0 < eq && *k0 == ' ')
                            ++k0;
                        const char *k1 = eq;
                        while (k1 > k0 && *(k1 - 1) == ' ')
                            --k1;
                        // trim value
                        const char *v0 = eq + 1;
                        while (v0 < end && *v0 == ' ')
                            ++v0;
                        const char *v1 = end;
                        while (v1 > v0 && *(v1 - 1) == ' ')
                            --v1;
                        if (k0 >= k1)
                            continue;
                        if (!first)
                            Serial.print(',');
                        first = false;
                        Serial.print('\"');
                        print_escaped(k0, k1);
                        Serial.print("\":");
                        bool quoted = (v1 > v0 && *v0 == '\"' && *(v1 - 1) == '\"');
                        if (quoted)
                        {
                            Serial.print('\"');
                            print_escaped(v0 + 1, v1 - 1);
                            Serial.print('\"');
                        }
                        else
                        {
                            // determine if numeric (not hex 0x..)
                            const char *t = v0;
                            bool numeric = true;
                            if (t < v1 && (*t == '+' || *t == '-'))
                                ++t;
                            if (t + 1 < v1 && *t == '0' && (t[1] == 'x' || t[1] == 'X'))
                            {
                                numeric = false; // keep hex as string
                            }
                            else
                            {
                                bool has_digit = false;
                                for (const char *q = t; q < v1; ++q)
                                {
                                    char c = *q;
                                    if ((c >= '0' && c <= '9') || c == '.')
                                    {
                                        has_digit = true;
                                        continue;
                                    }
                                    numeric = false;
                                    break;
                                }
                                if (!has_digit)
                                    numeric = false;
                            }
                            if (numeric)
                            {
                                while (v0 < v1)
                                    Serial.print(*v0++);
                            }
                            else
                            {
                                Serial.print('\"');
                                print_escaped(v0, v1);
                                Serial.print('\"');
                            }
                        }
                    }
                    Serial.println("}}");
                }
            };

            // err(): Send error response with error code
            // Supports both text and JSON formats
            auto err = [&](const char *msg)
            {
                if (!s_json_mode)
                {
                    Serial.print("ERR ");
                    Serial.println(msg ? msg : "UNKNOWN");
                }
                else
                {
                    Serial.print("{\"ok\":false,\"error\":{\"code\":\"");
                    Serial.print(msg ? msg : "UNKNOWN");
                    Serial.println("\",\"message\":\"\"}}");
                }
            };

            // ---- STEP 5: COMMAND DISPATCH ----
            // Route command to appropriate handler based on GROUP and ITEM tokens
            // Each GROUP (RDS, AUDIO, PILOT, SYST) has its own command handlers

            if (group_tok[0] && item_tok[0])
            {
                // ==================================================================================
                // RDS COMMAND GROUP: RDS:PI, RDS:PTY, RDS:PS, RDS:RT, RDS:ENABLE, RDS:STATUS, etc.
                // ==================================================================================
                if (iequal(group_tok, "RDS"))
                {
                    handled = true;
                    if (iequal(item_tok, "PI"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            unsigned v = 0;
                            if (strncmp(rest, "0x", 2) == 0 || strncmp(rest, "0X", 2) == 0)
                                v = (unsigned)strtoul(rest, nullptr, 16);
                            else
                                v = (unsigned)strtoul(rest, nullptr, 10);
                            RDSAssembler::setPI((uint16_t)(v & 0xFFFF));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "PI?"))
                    {
                        char b[32];
                        snprintf(b, sizeof(b), "PI=0x%04X", (unsigned)RDSAssembler::getPI());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "PTY"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            const char *rs = rest;
                            while (*rs == ' ' || *rs == '\t')
                                ++rs;
                            if (strncmp(rs, "LIST?", 5) == 0)
                            {
                                // Generate complete PTY list from map
                                struct P
                                {
                                    const char *n;
                                    uint8_t c;
                                };
                                static const P map[] = {{"NONE", 0},
                                                        {"NEWS", 1},
                                                        {"INFORMATION", 2},
                                                        {"SPORT", 3},
                                                        {"TALK", 4},
                                                        {"ROCK", 5},
                                                        {"CLASSIC_ROCK", 6},
                                                        {"ADULT_HITS", 7},
                                                        {"SOFT_ROCK", 8},
                                                        {"TOP_40", 10},
                                                        {"COUNTRY", 11},
                                                        {"OLDIES", 13},
                                                        {"SOFT", 14},
                                                        {"JAZZ", 15},
                                                        {"CLASSICAL", 16},
                                                        {"RNB", 17},
                                                        {"SOFT_RNB", 18},
                                                        {"LANGUAGE", 19},
                                                        {"RELIGIOUS_MUSIC", 20},
                                                        {"RELIGIOUS_TALK", 21},
                                                        {"PERSONALITY", 22},
                                                        {"PUBLIC", 24},
                                                        {"COLLEGE", 27}};
                                char list_buf[512];
                                list_buf[0] = '\0';
                                for (const auto &e : map)
                                {
                                    char entry[32];
                                    snprintf(entry, sizeof(entry), "%s%u=%s",
                                             list_buf[0] ? "," : "", e.c, e.n);
                                    strncat(list_buf, entry,
                                            sizeof(list_buf) - strlen(list_buf) - 1);
                                }
                                ok_kv(list_buf);
                                goto after_parse;
                            }
                            unsigned v = 0;
                            if (rs[0] >= '0' && rs[0] <= '9')
                                v = (unsigned)strtoul(rs, nullptr, 10);
                            else
                            {
                                struct P
                                {
                                    const char *n;
                                    uint8_t c;
                                };
                                static const P map[] = {{"NONE", 0},
                                                        {"NEWS", 1},
                                                        {"INFORMATION", 2},
                                                        {"SPORT", 3},
                                                        {"TALK", 4},
                                                        {"ROCK", 5},
                                                        {"CLASSIC_ROCK", 6},
                                                        {"ADULT_HITS", 7},
                                                        {"SOFT_ROCK", 8},
                                                        {"TOP_40", 10},
                                                        {"COUNTRY", 11},
                                                        {"OLDIES", 13},
                                                        {"SOFT", 14},
                                                        {"JAZZ", 15},
                                                        {"CLASSICAL", 16},
                                                        {"RNB", 17},
                                                        {"SOFT_RNB", 18},
                                                        {"LANGUAGE", 19},
                                                        {"RELIGIOUS_MUSIC", 20},
                                                        {"RELIGIOUS_TALK", 21},
                                                        {"PERSONALITY", 22},
                                                        {"PUBLIC", 24},
                                                        {"COLLEGE", 27}};
                                bool found = false;
                                for (auto &e : map)
                                {
                                    if (iequal(rs, e.n))
                                    {
                                        v = e.c;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                {
                                    err("BAD_VALUE");
                                    goto after_parse;
                                }
                            }
                            RDSAssembler::setPTY((uint8_t)(v & 0x1F));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "PTY?"))
                    {
                        char b[24];
                        snprintf(b, sizeof(b), "PTY=%u", (unsigned)RDSAssembler::getPTY());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "TP"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            RDSAssembler::setTP((atoi(rest) != 0));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "TP?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "TP=%u", (unsigned)RDSAssembler::getTP());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "TA"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            RDSAssembler::setTA((atoi(rest) != 0));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "TA?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "TA=%u", (unsigned)RDSAssembler::getTA());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "MS"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            RDSAssembler::setMS((atoi(rest) != 0));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "MS?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "MS=%u", (unsigned)RDSAssembler::getMS());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "PS"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            char text[64];
                            const char *rp = rest;
                            parse_quoted(rp, text, sizeof(text));
                            RDSAssembler::setPS(text);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "PS?"))
                    {
                        char ps[9];
                        RDSAssembler::getPS(ps);
                        trim_trailing_spaces(ps);
                        char b[32];
                        snprintf(b, sizeof(b), "PS=\"%s\"", ps);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "RT"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            char text[1024];
                            const char *rp = rest;
                            parse_quoted(rp, text, sizeof(text));
                            RDSAssembler::setRT(text);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "RT?"))
                    {
                        char rt[65];
                        RDSAssembler::getRT(rt);
                        trim_trailing_spaces(rt);
                        char b[96];
                        snprintf(b, sizeof(b), "RT=\"%s\"", rt);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "ENABLE"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setRdsEnable((atoi(rest) != 0));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "ENABLE?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "ENABLE=%u", DSP_pipeline::getRdsEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "STATUS?"))
                    {
                        char ps[9] = {0};
                        char rt[65] = {0};
                        RDSAssembler::getPS(ps);
                        RDSAssembler::getRT(rt);
                        trim_trailing_spaces(ps);
                        trim_trailing_spaces(rt);
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                                 "PI=0x%04X,PTY=%u,TP=%u,TA=%u,MS=%u,PS=\"%s\",RT=\"%s\",RTAB=%c,"
                                 "ENABLE=%u",
                                 (unsigned)RDSAssembler::getPI(), (unsigned)RDSAssembler::getPTY(),
                                 (unsigned)RDSAssembler::getTP(), (unsigned)RDSAssembler::getTA(),
                                 (unsigned)RDSAssembler::getMS(), ps, rt,
                                 RDSAssembler::getRTAB() ? 'B' : 'A',
                                 DSP_pipeline::getRdsEnable() ? 1 : 0);
                        ok_kv(buf);
                    }
                    else if (iequal(item_tok, "RTLIST?"))
                    {
                        // Support direct query form: RDS:RTLIST?
                        if (!s_json_mode)
                        {
                            char line[512];
                            line[0] = '\0';
                            bool first = true;
                            for (std::size_t i = 0; i < RDSAssembler::rtListCount(); ++i)
                            {
                                char t[128];
                                if (RDSAssembler::rtListGet(i, t, sizeof(t)))
                                {
                                    char part[160];
                                    snprintf(part, sizeof(part), "%s%u=\"%s\"", first ? "" : ",",
                                             (unsigned)i, t);
                                    strncat(line, part, sizeof(line) - strlen(line) - 1);
                                    first = false;
                                }
                            }
                            ok_kv(line);
                        }
                        else
                        {
                            Serial.print("{\\\"ok\\\":true,\\\"data\\\":{\\\"RTLIST\\\":[");
                            bool first = true;
                            for (std::size_t i = 0; i < RDSAssembler::rtListCount(); ++i)
                            {
                                char t[128];
                                if (RDSAssembler::rtListGet(i, t, sizeof(t)))
                                {
                                    if (!first)
                                        Serial.print(',');
                                    first = false;
                                    Serial.print('\"');
                                    for (const char *s = t; *s; ++s)
                                    {
                                        char c = *s;
                                        if (c == '"' || c == '\\')
                                        {
                                            Serial.print('\\');
                                            Serial.print(c);
                                        }
                                        else if ((unsigned char)c < 0x20)
                                        {
                                            char buf[8];
                                            snprintf(buf, sizeof(buf), "\\u%04X",
                                                     (unsigned)(unsigned char)c);
                                            Serial.print(buf);
                                        }
                                        else
                                        {
                                            Serial.print(c);
                                        }
                                    }
                                    Serial.print('\"');
                                }
                            }
                            Serial.println("]}}");
                        }
                    }
                    else if (iequal(item_tok, "RTLIST"))
                    {
                        // subcommands: ADD/DEL/CLEAR/?
                        char sub[16];
                        sub[0] = '\0';
                        const char *sp2 = rest;
                        next_token(sp2, sub, sizeof(sub));
                        if (iequal(sub, "ADD"))
                        {
                            if (!sp2 || !*sp2)
                            {
                                err("MISSING_ARG");
                            }
                            else
                            {
                                char text[512];
                                const char *rp = sp2;
                                parse_quoted(rp, text, sizeof(text));
                                RDSAssembler::rtListAdd(text);
                                ok();
                            }
                        }
                        else if (iequal(sub, "DEL"))
                        {
                            if (!sp2 || !*sp2)
                            {
                                err("MISSING_ARG");
                            }
                            else
                            {
                                unsigned idx = (unsigned)strtoul(sp2, nullptr, 10);
                                if (!RDSAssembler::rtListDel(idx))
                                    err("BAD_INDEX");
                                else
                                    ok();
                            }
                        }
                        else if (iequal(sub, "CLEAR"))
                        {
                            RDSAssembler::rtListClear();
                            ok();
                        }
                        else if (iequal(sub, "?"))
                        {
                            if (!s_json_mode)
                            {
                                // Text mode: OK 0="...",1="..."
                                char line[512];
                                line[0] = '\0';
                                bool first = true;
                                for (std::size_t i = 0; i < RDSAssembler::rtListCount(); ++i)
                                {
                                    char t[128];
                                    if (RDSAssembler::rtListGet(i, t, sizeof(t)))
                                    {
                                        char part[160];
                                        snprintf(part, sizeof(part), "%s%u=\"%s\"",
                                                 first ? "" : ",", (unsigned)i, t);
                                        strncat(line, part, sizeof(line) - strlen(line) - 1);
                                        first = false;
                                    }
                                }
                                ok_kv(line);
                            }
                            else
                            {
                                // JSON mode: {"ok":true,"data":{"RTLIST":["a","b"]}}
                                Serial.print("{\"ok\":true,\"data\":{\"RTLIST\":[");
                                bool first = true;
                                for (std::size_t i = 0; i < RDSAssembler::rtListCount(); ++i)
                                {
                                    char t[128];
                                    if (RDSAssembler::rtListGet(i, t, sizeof(t)))
                                    {
                                        if (!first)
                                            Serial.print(',');
                                        first = false;
                                        // Escape string minimally for JSON
                                        Serial.print('\"');
                                        for (const char *s = t; *s; ++s)
                                        {
                                            char c = *s;
                                            if (c == '"' || c == '\\')
                                            {
                                                Serial.print('\\');
                                                Serial.print(c);
                                            }
                                            else if ((unsigned char)c < 0x20)
                                            {
                                                char buf[8];
                                                snprintf(buf, sizeof(buf), "\\u%04X",
                                                         (unsigned)(unsigned char)c);
                                                Serial.print(buf);
                                            }
                                            else
                                            {
                                                Serial.print(c);
                                            }
                                        }
                                        Serial.print('\"');
                                    }
                                }
                                Serial.println("]}}");
                            }
                        }
                        else
                        {
                            err("Unknown RDS item");
                        }
                    }
                    else if (iequal(item_tok, "RTPERIOD"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            unsigned s = (unsigned)strtoul(rest, nullptr, 10);
                            RDSAssembler::setRtPeriod(s);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "RTPERIOD?"))
                    {
                        char b[32];
                        snprintf(b, sizeof(b), "RTPERIOD=%u",
                                 (unsigned)RDSAssembler::getRtPeriod());
                        ok_kv(b);
                    }
                    else
                    {
                        err("Unknown RDS item");
                    }
                }
                // ==================================================================================
                // AUDIO COMMAND GROUP: AUDIO:STEREO, AUDIO:PREEMPH, AUDIO:STATUS
                // ==================================================================================
                else if (iequal(group_tok, "AUDIO"))
                {
                    handled = true;
                    if (iequal(item_tok, "STEREO"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setStereoEnable(atoi(rest) != 0);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "STEREO?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "STEREO=%u",
                                 DSP_pipeline::getStereoEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "PREEMPH"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setPreemphEnable(atoi(rest) != 0);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "PREEMPH?"))
                    {
                        char b[20];
                        snprintf(b, sizeof(b), "PREEMPH=%u",
                                 DSP_pipeline::getPreemphEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "STATUS?"))
                    {
                        char b[64];
                        snprintf(b, sizeof(b), "STEREO=%u,PREEMPH=%u",
                                 DSP_pipeline::getStereoEnable() ? 1 : 0,
                                 DSP_pipeline::getPreemphEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else
                    {
                        err("Unknown AUDIO item");
                    }
                }
                // ==================================================================================
                // PILOT COMMAND GROUP: PILOT:ENABLE, PILOT:AUTO, PILOT:THRESH, PILOT:HOLD
                // ==================================================================================
                else if (iequal(group_tok, "PILOT"))
                {
                    handled = true;
                    if (iequal(item_tok, "ENABLE"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setPilotEnable(atoi(rest) != 0);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "ENABLE?"))
                    {
                        char b[20];
                        snprintf(b, sizeof(b), "ENABLE=%u", DSP_pipeline::getPilotEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "AUTO"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setPilotAuto(atoi(rest) != 0);
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "AUTO?"))
                    {
                        char b[16];
                        snprintf(b, sizeof(b), "AUTO=%u", DSP_pipeline::getPilotAuto() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "THRESH"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setPilotThresh((float)atof(rest));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "THRESH?"))
                    {
                        char b[32];
                        snprintf(b, sizeof(b), "THRESH=%g", (double)DSP_pipeline::getPilotThresh());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "HOLD"))
                    {
                        if (!rest || !*rest)
                        {
                            err("MISSING_ARG");
                        }
                        else
                        {
                            DSP_pipeline::setPilotHold((uint32_t)strtoul(rest, nullptr, 10));
                            ok();
                        }
                    }
                    else if (iequal(item_tok, "HOLD?"))
                    {
                        char b[32];
                        snprintf(b, sizeof(b), "HOLD=%u", (unsigned)DSP_pipeline::getPilotHold());
                        ok_kv(b);
                    }
                    else
                    {
                        err("Unknown PILOT item");
                    }
                }
                // ==================================================================================
                // SYST COMMAND GROUP: SYST:VERSION, SYST:STATUS, SYST:HEAP, SYST:LOG, SYST:CONF,
                // etc. System commands for monitoring and configuration management
                // ==================================================================================
                else if (iequal(group_tok, "SYST"))
                {
                    handled = true;
                    if (iequal(item_tok, "VERS") || iequal(item_tok, "VERSION?"))
                    {
                        char b[128];
                        snprintf(b, sizeof(b), "VERSION=%s,BUILD=%s,BUILDTIME=%s %s",
                                 Config::FIRMWARE_VERSION, __DATE__, __TIME__, "");
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "HELP") || iequal(item_tok, "HELP?"))
                    {
                        char topic[16];
                        topic[0] = '\0';
                        if (rest && *rest)
                        {
                            const char *rp = rest;
                            next_token(rp, topic, sizeof(topic));
                        }
                        if (topic[0] == '\0')
                        {
                            Serial.println("OK TOPICS=RDS,AUDIO,PILOT,SYST");
                        }
                        else if (iequal(topic, "RDS"))
                        {
                            Serial.println(
                                "OK RDS PI|PI? PTY|PTY? TP|TP? TA|TA? MS|MS? PS|PS? RT|RT? "
                                "ENABLE|ENABLE? RTLIST:ADD|DEL|CLEAR|? RTPERIOD|RTPERIOD? STATUS?");
                        }
                        else if (iequal(topic, "AUDIO"))
                        {
                            Serial.println("OK AUDIO STEREO|STEREO? PREEMPH|PREEMPH? STATUS?");
                        }
                        else if (iequal(topic, "PILOT"))
                        {
                            Serial.println(
                                "OK PILOT ENABLE|ENABLE? AUTO|AUTO? THRESH|THRESH? HOLD|HOLD?");
                        }
                        else if (iequal(topic, "SYST"))
                        {
                            Serial.println("OK SYST VERSION? STATUS? HEAP? LOG:LEVEL|LOG:LEVEL? "
                                           "COMM:JSON|COMM:JSON? "
                                           "CONF:SAVE|CONF:LOAD|CONF:LIST?|CONF:ACTIVE?|CONF:"
                                           "DELETE CONF:DEFAULT DEFAULTS REBOOT");
                        }
                        else
                        {
                            Serial.println("OK");
                        }
                    }
                    // Nested forms: SYST:LOG:LEVEL and SYST:COMM:JSON
                    else if (iequal(item_tok, "LOG"))
                    {
                        char sub[16];
                        sub[0] = '\0';
                        const char *rp = rest;
                        next_token(rp, sub, sizeof(sub));
                        if (iequal(sub, "LEVEL"))
                        {
                            char tok[16];
                            tok[0] = '\0';
                            next_token(rp, tok, sizeof(tok));
                            if (!tok[0])
                            {
                                err("MISSING_ARG");
                            }
                            else if (iequal(tok, "OFF"))
                            {
                                // If we're still in startup, defer muting until startup completes
                                if (s_startup_phase)
                                {
                                    s_mute_after_startup = true;
                                    s_log_mute = false; // keep startup logs visible
                                }
                                else
                                {
                                    s_log_mute = true;
                                    s_mute_after_startup = false;
                                }
                                ok();
                            }
                            else
                            {
                                s_log_mute = false;
                                if (iequal(tok, "ERROR"))
                                    s_min_level = LogLevel::ERROR;
                                else if (iequal(tok, "WARN"))
                                    s_min_level = LogLevel::WARN;
                                else if (iequal(tok, "INFO"))
                                    s_min_level = LogLevel::INFO;
                                else
                                    s_min_level = LogLevel::DEBUG;
                                ok();
                            }
                        }
                        else if (iequal(sub, "LEVEL?"))
                        {
                            const char *lvl = s_log_mute                         ? "OFF"
                                              : (s_min_level == LogLevel::ERROR) ? "ERROR"
                                              : (s_min_level == LogLevel::WARN)  ? "WARN"
                                              : (s_min_level == LogLevel::INFO)  ? "INFO"
                                                                                 : "DEBUG";
                            char b[24];
                            snprintf(b, sizeof(b), "LEVEL=%s", lvl);
                            ok_kv(b);
                        }
                        else
                        {
                            err("Unknown SYST LOG item");
                        }
                    }
                    else if (iequal(item_tok, "COMM"))
                    {
                        char sub[16];
                        sub[0] = '\0';
                        const char *rp = rest;
                        next_token(rp, sub, sizeof(sub));
                        if (iequal(sub, "JSON"))
                        {
                            char tok[8];
                            tok[0] = '\0';
                            next_token(rp, tok, sizeof(tok));
                            if (!tok[0])
                            {
                                err("MISSING_ARG");
                            }
                            else
                            {
                                s_json_mode = (atoi(tok) != 0) || iequal(tok, "ON");
                                ok();
                            }
                        }
                        else if (iequal(sub, "JSON?"))
                        {
                            char b[16];
                            snprintf(b, sizeof(b), "JSON=%u", s_json_mode ? 1 : 0);
                            ok_kv(b);
                        }
                        else
                        {
                            err("Unknown SYST COMM item");
                        }
                    }
                    else if (iequal(item_tok, "STATUS?"))
                    {
                        float core0 = 0, core1 = 0, aud = 0, logg = 0, vu = 0;
                        uint32_t a_sw = 0, l_sw = 0, v_sw = 0;
                        TaskStats::collect(core0, core1, aud, logg, vu, a_sw, l_sw, v_sw);
                        char b[160];
                        snprintf(b, sizeof(b),
                                 "UPTIME=%u,CPU=%.1f,CORE0=%.1f,CORE1=%.1f,HEAP_FREE=%u,HEAP_MIN=%"
                                 "u,STEREO=%u,AUDIO_CLIPPING=0",
                                 (unsigned)(esp_timer_get_time() / 1000000ULL), (double)aud,
                                 (double)core0, (double)core1, (unsigned)ESP.getFreeHeap(),
                                 (unsigned)ESP.getMinFreeHeap(),
                                 DSP_pipeline::getStereoEnable() ? 1 : 0);
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "HEAP?"))
                    {
                        char b[64];
                        snprintf(b, sizeof(b), "CURRENT_FREE=%u,MIN_FREE=%u",
                                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
                        ok_kv(b);
                    }
                    else if (iequal(item_tok, "CONF"))
                    {
                        // Handle nested CONF:* commands by parsing subcommand from rest
                        char sub[16];
                        sub[0] = '\0';
                        const char *rp = rest;
                        next_token(rp, sub, sizeof(sub));

                        if (iequal(sub, "SAVE"))
                        {
                            // Save current settings into named profile
                            char name[32] = {0};
                            if (rp && *rp)
                            {
                                next_token(rp, name, sizeof(name));
                            }
                            if (name[0] == '\0')
                                strncpy(name, "default", sizeof(name) - 1);
                            conf_open_rw();
                            char blob[1024];
                            conf_build_blob(blob, sizeof(blob));
                            String key = String("p:") + name;
                            bool okput = s_prefs.putString(key.c_str(), blob) > 0;
                            if (okput)
                            {
                                String list = s_prefs.getString("_list", "");
                                list = strlist_add_unique(list, name);
                                s_prefs.putString("_list", list);
                                s_prefs.putString("_active", name);
                                ok();
                            }
                            else
                            {
                                err("SAVE_FAILED");
                            }
                            conf_close();
                        }
                        else if (iequal(sub, "LOAD"))
                        {
                            char name[32] = {0};
                            if (rp && *rp)
                            {
                                next_token(rp, name, sizeof(name));
                            }
                            if (name[0] == '\0')
                                strncpy(name, "default", sizeof(name) - 1);
                            conf_open_rw();
                            String key = String("p:") + name;
                            String blob = s_prefs.getString(key.c_str(), "");
                            if (blob.length() == 0)
                            {
                                conf_close();
                                err("NOT_FOUND");
                            }
                            else
                            {
                                apply_loaded_blob(blob.c_str());
                                s_prefs.putString("_active", name);
                                conf_close();
                                ok();
                            }
                        }
                        else if (iequal(sub, "LIST?"))
                        {
                            conf_open_rw();
                            String list = s_prefs.getString("_list", "");
                            conf_close();
                            if (!s_json_mode)
                            {
                                char b[256];
                                snprintf(b, sizeof(b), "LIST=%s", list.c_str());
                                ok_kv(b);
                            }
                            else
                            {
                                Serial.print("{\\\"ok\\\":true,\\\"data\\\":{\\\"LIST\\\":[");
                                bool first = true;
                                int start = 0;
                                while (start >= 0)
                                {
                                    int comma = list.indexOf(',', start);
                                    String token = (comma < 0) ? list.substring(start)
                                                               : list.substring(start, comma);
                                    token.trim();
                                    if (token.length() > 0)
                                    {
                                        if (!first)
                                            Serial.print(',');
                                        first = false;
                                        Serial.print('\"');
                                        for (size_t i = 0; i < token.length(); ++i)
                                        {
                                            char c = token[i];
                                            if (c == '"' || c == '\\')
                                            {
                                                Serial.print('\\');
                                                Serial.print(c);
                                            }
                                            else if ((unsigned char)c < 0x20)
                                            {
                                                char buf[8];
                                                snprintf(buf, sizeof(buf), "\\u%04X",
                                                         (unsigned)(unsigned char)c);
                                                Serial.print(buf);
                                            }
                                            else
                                            {
                                                Serial.print(c);
                                            }
                                        }
                                        Serial.print('\"');
                                    }
                                    if (comma < 0)
                                        break;
                                    else
                                        start = comma + 1;
                                }
                                Serial.println("]}}");
                            }
                        }
                        else if (iequal(sub, "ACTIVE?"))
                        {
                            conf_open_rw();
                            String act = s_prefs.getString("_active", "");
                            conf_close();
                            char b[96];
                            snprintf(b, sizeof(b), "ACTIVE=\"%s\"", act.c_str());
                            ok_kv(b);
                        }
                        else if (iequal(sub, "DELETE"))
                        {
                            if (!rp || !*rp)
                            {
                                err("MISSING_ARG");
                            }
                            else
                            {
                                char name[32] = {0};
                                next_token(rp, name, sizeof(name));
                                conf_open_rw();
                                String key = String("p:") + name;
                                bool removed = s_prefs.remove(key.c_str());
                                String list = s_prefs.getString("_list", "");
                                list = strlist_remove(list, name);
                                s_prefs.putString("_list", list);
                                String act = s_prefs.getString("_active", "");
                                if (act == String(name))
                                    s_prefs.putString("_active", "");
                                conf_close();
                                if (removed)
                                    ok();
                                else
                                    err("NOT_FOUND");
                            }
                        }
                        else if (iequal(sub, "DEFAULT"))
                        {
                            apply_factory_defaults();
                            ok();
                        }
                        else
                        {
                            err("Unknown SYST CONF item");
                        }
                    }
                    else if (iequal(item_tok, "DEFAULTS"))
                    {
                        // Alias to CONF:DEFAULT
                        apply_factory_defaults();
                        ok();
                    }
                    else if (iequal(item_tok, "REBOOT"))
                    {
                        ok();
                        delay(50);
                        ESP.restart();
                    }
                    else
                    {
                        err("Unknown SYST item");
                    }
                }
            }

            if (!handled && line[0] != '\0')
            {
                err("Unknown command");
            }

        after_parse:
            // reset buffer for next line (MUST be after label for goto to work)
            line_len = 0;
        }
        else
        {
            if (line_len < sizeof(line_buf) - 1)
                line_buf[line_len++] = ch;
        }
    }

    // Small sleep to avoid busy loop
    vTaskDelay(pdMS_TO_TICKS(1));
}

/**
 * Shutdown Module Resources (ModuleBase contract)
 *
 * Called during graceful shutdown. Cleans up queue resources.
 */
void Console::shutdown()
{
    if (queue_ != nullptr)
    {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

// ==================================================================================
//                          INSTANCE MESSAGE ENQUEUE METHODS
// ==================================================================================

/**
 * Instance Method - Enqueue Preformatted Message
 *
 * Core implementation of static enqueue(). Constructs a LogMsg structure
 * and attempts non-blocking insertion into the queue.
 */
bool Console::enqueueRaw(LogLevel level, const char *msg)
{
    // ---- Validate Input Parameters ----
    if (!queue_ || !msg)
    {
        return false;
    }

    // ---- Construct Log Message ----
    LogMsg m{};
    m.ts_us = micros();                    // Capture current timestamp
    m.level = static_cast<uint8_t>(level); // Store log level

    // ---- Copy Message Text with Bounds Checking ----
    size_t i = 0;
    while (msg[i] && i < sizeof(m.text) - 1) // Leave room for null terminator
    {
        m.text[i] = msg[i];
        ++i;
    }
    m.text[i] = '\0'; // Ensure null termination

    // ---- Attempt Non-Blocking Enqueue ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        // Queue full - increment drop counter and return failure
        dropped_count_++;
        return false;
    }

    return true;
}

/**
 * Instance Method - Enqueue Formatted Message
 *
 * Core implementation of static enqueuef(). Formats the message using
 * vsnprintf() and attempts non-blocking insertion into the queue.
 */
bool Console::enqueueFormatted(LogLevel level, const char *fmt, va_list ap)
{
    // ---- Validate Input Parameters ----
    if (!queue_ || !fmt)
    {
        return false;
    }

    // ---- Construct Log Message ----
    LogMsg m{};
    m.ts_us = micros();                    // Capture current timestamp
    m.level = static_cast<uint8_t>(level); // Store log level

    // ---- Format Message Text ----
    vsnprintf(m.text, sizeof(m.text), fmt, ap); // Format with bounds checking

    // ---- Attempt Non-Blocking Enqueue ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        // Queue full - increment drop counter and return failure
        dropped_count_++;
        return false;
    }

    return true;
}
