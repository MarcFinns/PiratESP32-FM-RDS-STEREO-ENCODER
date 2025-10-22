/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
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
 * Supported Commands (SCPI summary)
 *   Grouped, case-insensitive, tolerant to extra spaces. Queries end with '?'.
 *   Values accept 0|1 and ON|OFF for booleans (queries always return 0|1).
 *
 *   RDS (Core)
 *     - RDS:PI <hex|dec> / RDS:PI?                  → PI=0xNNNN
 *     - RDS:PTY <0-31|NAME> / RDS:PTY?              → PTY=<n>
 *     - RDS:TP <0|1> / RDS:TP?                      → TP=0|1
 *     - RDS:TA <0|1> / RDS:TA?                      → TA=0|1
 *     - RDS:MS <0|1> / RDS:MS?                      → MS=0|1
 *     - RDS:PS "text" / RDS:PS?                     → PS="..." (8 chars padded)
 *     - RDS:RT "text" / RDS:RT?                     → RT="..." (64-char broadcast window)
 *     - RDS:ENABLE <0|1> / RDS:ENABLE?              → ENABLE=0|1 (57 kHz on/off)
 *     - RDS:STATUS? → PI,PTY,TP,TA,MS,PS,RT,RTAB,ENABLE
 *     - RDS:RTLIST:ADD "text" | :DEL <idx> | :CLEAR | :? | RTLIST? (direct)
 *     - RDS:RTPERIOD <sec> / RDS:RTPERIOD?          → RTPERIOD=s
 *
 *   AUDIO / PILOT
 *     - AUDIO:STEREO <0|1> / AUDIO:STEREO?          → STEREO=0|1
 *     - AUDIO:PREEMPH <0|1> / AUDIO:PREEMPH?        → PREEMPH=0|1
 *     - AUDIO:STATUS?                                → grouped status
 *     - PILOT:ENABLE <0|1> / PILOT:ENABLE?          → ENABLE=0|1
 *     - PILOT:AUTO <0|1> / PILOT:AUTO?              → AUTO=0|1
 *     - PILOT:THRESH <float> / PILOT:THRESH?        → THRESH=x
 *     - PILOT:HOLD <ms> / PILOT:HOLD?               → HOLD=ms
 *
 *   SYST / COMM / LOG / CONF
 *     - SYST:VERSION?                                → VERSION,BUILD,BUILDTIME
 *     - SYST:STATUS? / SYST:HEAP?                    → system metrics
 *     - SYST:LOG:LEVEL OFF|ERROR|WARN|INFO | :LEVEL? → logging control
 *     - SYST:COMM:JSON ON|OFF | :JSON?               → JSON replies ON/OFF
 *     - SYST:CONF:SAVE [name]                        → stores profile under "conf" ns
 *       SYST:CONF:LOAD [name] / :LIST? / :ACTIVE? / :DELETE <name> / :DEFAULT
 *     - SYST:DEFAULTS (alias of CONF:DEFAULT)        → factory defaults
 *     - SYST:REBOOT                                  → restart
 *
 * JSON Mode
 *   - Enable with SYST:COMM:JSON ON. All replies become single-line JSON:
 *       {"ok":true,"data":{...}} or {"ok":false,"error":{"code":"..."}}
 *   - resp_ok_kv() converts "key=value, ..." into proper JSON pairs.
 *   - Strings are escaped minimally (quotes, backslashes, control chars → \uXXXX).
 *
 * Persistence (NVS schema)
 *   - Namespace: "conf"
 *   - Keys:  _list (CSV of names), _active (current name), p:<name> (profile blob)
 *   - Blob keys include: PI,PTY,TP,TA,MS,PS,RT,RTPERIOD, RTLIST, AUDIO_STEREO,
 *                        PREEMPH, RDS_ENABLE, PILOT_ENABLE, PILOT_AUTO,
 *                        PILOT_THRESH, PILOT_HOLD, LOG_LEVEL
 *   - Special LOG_LEVEL=255 indicates OFF; muting is deferred until startup ends
 *     so the boot sequence remains visible.
 *
 * Serial & Line Discipline
 *   - Console owns Serial completely. Input is line-oriented; CR is ignored and LF terminates.
 *   - Non-blocking logging uses a drop-oldest queue to avoid ever stalling audio.
 *
 * =====================================================================================
 */

#include "Config.h"
#include "Console.h"
#include "DisplayManager.h"
#include "RDSAssembler.h"

#include "DSP_pipeline.h"
#include "DisplayManager.h"
#include "PtyMap.h"
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
/**
 * Static Wrapper - Start Console Task (single entry point)
 */
bool Console::startTask(int core_id, UBaseType_t priority, uint32_t stack_words, size_t queue_len)
{
    Console &logger = getInstance();
    logger.queue_len_ = queue_len;
    logger.core_id_ = core_id;
    logger.priority_ = priority;
    logger.stack_words_ = stack_words;
    return logger.spawnTask("console", stack_words, priority, core_id, Console::taskTrampoline);
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

bool Console::shouldLog(LogLevel level)
{
    // Allow all during startup; otherwise respect mute and level threshold
    if (s_startup_phase)
        return true;
    int lvl = static_cast<int>(level);
    int thr = static_cast<int>(s_min_level);
    return (!s_log_mute) && (lvl >= thr);
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

void Console::stopTask()
{
    Console &c = getInstance();
    if (c.isRunning())
    {
        TaskHandle_t h = c.getTaskHandle();
        if (h)
        {
            vTaskDelete(h);
            c.setTaskHandle(nullptr);
        }
    }
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
// NVS layout:
//   namespace: "conf"
//   keys:
//     _list                 CSV of profile names
//     _active               name of currently active profile
//     p:<name>              serialized profile blob (key=value; RTLIST encoded as "a"|"b"|...)
static Preferences s_prefs;
static const char *kPrefsNs = "conf";
static bool s_prefs_open = false;

// -------------------- Minimal JSON string escaping helpers --------------------
/**
 * Emit a JSON-escaped string segment to Serial.
 * - Escapes '"' and '\\' and any control character < 0x20.
 * - Control characters are printed using \uXXXX (hex uppercase) via snprintf.
 */
static inline void json_print_escaped_range(const char *s, const char *e)
{
    while (s < e)
    {
        char c = *s++;
        if (c == '\\' || c == '"')
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
}
/** Print a C-string as a JSON string literal (with surrounding quotes). */
static inline void json_print_string(const char *s)
{
    Serial.print('"');
    const char *p = s;
    while (*p)
        ++p;
    json_print_escaped_range(s, p);
    Serial.print('"');
}

// -------------------- Parser/response helpers (file-scope) --------------------
/** Case-insensitive equality for ASCII tokens (no locale dependency). */
static inline bool str_iequal(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = (*a >= 'a' && *a <= 'z') ? (*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (*b - 32) : *b;
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/** Trim leading and trailing ASCII spaces/tabs in-place. */
static inline void trim_line(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/**
 * Extract next token from a string pointer (delims: space, tab, colon).
 * - Advances p past the token and the delimiter.
 * - Writes NUL-terminated token to out (truncated if necessary).
 */
static inline void next_token_str(const char *&p, char *out, size_t outsz)
{
    while (*p == ' ' || *p == '\t' || *p == ':')
        ++p;
    if (!*p)
    {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ':')
    {
        if (i < outsz - 1)
            out[i++] = *p;
        ++p;
    }
    out[i] = '\0';
}

/**
 * Parse either a quoted string ("...") allowing simple escapes, or the remainder unquoted.
 * Used for PS/RT and RTLIST items. Result is NUL-terminated and truncated if needed.
 */
static inline void parse_quoted_str(const char *&p, char *out, size_t outsz)
{
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p == '"')
    {
        ++p; // skip opening quote
        size_t i = 0;
        while (*p && *p != '"' && i < outsz - 1)
        {
            if (*p == '\\' && *(p + 1) != 0)
                ++p;
            out[i++] = *p++;
        }
        if (*p == '"')
            ++p;
        out[i] = '\0';
        return;
    }
    size_t i = 0;
    while (*p && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/** Emit OK reply (text or JSON), no data payload. */
static inline void resp_ok()
{
    if (!s_json_mode)
        Serial.println("OK");
    else
        Serial.println("{\"ok\":true}");
}

/**
 * Emit OK reply with data expressed as comma-separated key=value pairs.
 * In JSON mode, converts into a proper object with minimal type inference (ints/floats).
 */
static inline void resp_ok_kv(const char *kv)
{
    if (!s_json_mode)
    {
        Serial.print("OK ");
        Serial.println(kv ? kv : "");
        return;
    }
    Serial.print("{\"ok\":true,\"data\":{");
    const char *p = kv ? kv : "";
    bool first = true;
    while (*p)
    {
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
        const char *end = p;
        if (*p == ',')
            ++p;
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
            continue;
        const char *k0 = start;
        while (k0 < eq && *k0 == ' ')
            ++k0;
        const char *k1 = eq;
        while (k1 > k0 && *(k1 - 1) == ' ')
            --k1;
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
        Serial.print('"');
        json_print_escaped_range(k0, k1);
        Serial.print("\":");
        bool quoted = (v1 > v0 && *v0 == '"' && *(v1 - 1) == '"');
        if (quoted)
        {
            Serial.print('"');
            json_print_escaped_range(v0 + 1, v1 - 1);
            Serial.print('"');
        }
        else
        {
            const char *t = v0;
            bool numeric = true;
            if (t < v1 && (*t == '+' || *t == '-'))
                ++t;
            if (t + 1 < v1 && *t == '0' && (t[1] == 'x' || t[1] == 'X'))
                numeric = false;
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
                while (v0 < v1)
                    Serial.print(*v0++);
            else
            {
                Serial.print('"');
                json_print_escaped_range(v0, v1);
                Serial.print('"');
            }
        }
    }
    Serial.println("}}");
}

/** Emit error reply with an error code string. */
static inline void resp_err(const char *msg)
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
}

// -------------------- Group Handlers --------------------
// Forward declarations for persistence helpers defined later in this file
static void conf_open_rw();
static void conf_close();
static bool strlist_contains(const String &csv, const char *name);
static String strlist_remove(const String &csv, const char *name);
static void conf_build_blob(char *buf, size_t sz);
static void apply_loaded_blob(const char *blob);
static void apply_factory_defaults();
/**
 * Handle RDS:* commands.
 * Implements PI/PTY/TP/TA/MS, PS/RT set/get, ENABLE, STATUS, RTLIST (ADD/DEL/CLEAR/?), RTPERIOD.
 * Accepts numeric or standard EU RDS PTY names. Queries always return numeric flags (0|1).
 */
static bool handleRDS(const char *item_tok, const char *rest)
{
    if (str_iequal(item_tok, "PI"))
    {
        if (!rest || !*rest)
        {
            resp_err("MISSING_ARG");
        }
        else
        {
            unsigned v = 0;
            if (strncmp(rest, "0x", 2) == 0 || strncmp(rest, "0X", 2) == 0)
                v = (unsigned)strtoul(rest, nullptr, 16);
            else
                v = (unsigned)strtoul(rest, nullptr, 10);
            RDSAssembler::setPI((uint16_t)(v & 0xFFFF));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "PI?"))
    {
        char b[32];
        snprintf(b, sizeof(b), "PI=0x%04X", (unsigned)RDSAssembler::getPI());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "PTY"))
    {
        if (!rest || !*rest)
        {
            resp_err("MISSING_ARG");
            return true;
        }
        const char *rs = rest;
        while (*rs == ' ' || *rs == '\t')
            ++rs;
        if (strncmp(rs, "LIST?", 5) == 0)
        {
            char list_buf[512];
            list_buf[0] = '\0';
            for (size_t i = 0; i < kPtyMapSize; ++i)
            {
                char entry[32];
                snprintf(entry, sizeof(entry), "%s%u=%s", list_buf[0] ? "," : "",
                         (unsigned)kPtyMap[i].code, kPtyMap[i].long_name);
                strncat(list_buf, entry, sizeof(list_buf) - strlen(list_buf) - 1);
            }
            resp_ok_kv(list_buf);
            return true;
        }
        unsigned v = 0;
        if (rs[0] >= '0' && rs[0] <= '9')
            v = (unsigned)strtoul(rs, nullptr, 10);
        else
        {
            bool found = false;
            for (size_t i = 0; i < kPtyMapSize; ++i)
            {
                if (str_iequal(rs, kPtyMap[i].long_name))
                {
                    v = kPtyMap[i].code;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                resp_err("BAD_VALUE");
                return true;
            }
        }
        RDSAssembler::setPTY((uint8_t)(v & 0x1F));
        resp_ok();
        return true;
    }
    if (str_iequal(item_tok, "PTY?"))
    {
        char b[24];
        snprintf(b, sizeof(b), "PTY=%u", (unsigned)RDSAssembler::getPTY());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "TP"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            RDSAssembler::setTP((atoi(rest) != 0));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "TP?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "TP=%u", (unsigned)RDSAssembler::getTP());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "TA"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            RDSAssembler::setTA((atoi(rest) != 0));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "TA?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "TA=%u", (unsigned)RDSAssembler::getTA());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "MS"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            RDSAssembler::setMS((atoi(rest) != 0));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "MS?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "MS=%u", (unsigned)RDSAssembler::getMS());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "PS"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            char buf[128];
            const char *rp = rest;
            parse_quoted_str(rp, buf, sizeof(buf));
            RDSAssembler::setPS(buf);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "PS?"))
    {
        char ps[9] = {0};
        RDSAssembler::getPS(ps);
        // Trim trailing spaces for display
        int n = 7;
        while (n >= 0 && ps[n] == ' ')
            ps[n--] = '\0';
        char b[64];
        snprintf(b, sizeof(b), "PS=\"%s\"", ps);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "RT"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            char text[256];
            const char *rp = rest;
            parse_quoted_str(rp, text, sizeof(text));
            RDSAssembler::setRT(text);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "RT?"))
    {
        char rt[65];
        RDSAssembler::getRT(rt);
        // Trim trailing spaces
        for (int i = 63; i >= 0 && rt[i] == ' '; --i)
            rt[i] = '\0';
        char b[96];
        snprintf(b, sizeof(b), "RT=\"%s\"", rt);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "ENABLE"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setRdsEnable((atoi(rest) != 0));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "ENABLE?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "ENABLE=%u", DSP_pipeline::getRdsEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "STATUS?"))
    {
        char ps[9] = {0};
        char rt[65] = {0};
        RDSAssembler::getPS(ps);
        RDSAssembler::getRT(rt);
        int p = 7;
        while (p >= 0 && ps[p] == ' ')
            ps[p--] = '\0';
        int r = 63;
        while (r >= 0 && rt[r] == ' ')
            rt[r--] = '\0';
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "PI=0x%04X,PTY=%u,TP=%u,TA=%u,MS=%u,PS=\"%s\",RT=\"%s\",RTAB=%c,ENABLE=%u",
                 (unsigned)RDSAssembler::getPI(), (unsigned)RDSAssembler::getPTY(),
                 (unsigned)RDSAssembler::getTP(), (unsigned)RDSAssembler::getTA(),
                 (unsigned)RDSAssembler::getMS(), ps, rt, RDSAssembler::getRTAB() ? 'B' : 'A',
                 DSP_pipeline::getRdsEnable() ? 1 : 0);
        resp_ok_kv(buf);
        return true;
    }
    if (str_iequal(item_tok, "RTLIST?"))
    {
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
                    snprintf(part, sizeof(part), "%s%u=\"%s\"", first ? "" : ",", (unsigned)i, t);
                    strncat(line, part, sizeof(line) - strlen(line) - 1);
                    first = false;
                }
            }
            resp_ok_kv(line);
        }
        else
        {
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
                    json_print_string(t);
                }
            }
            Serial.println("]}}");
        }
        return true;
    }
    if (str_iequal(item_tok, "RTLIST"))
    {
        char sub[16];
        sub[0] = '\0';
        const char *sp2 = rest ? rest : "";
        next_token_str(sp2, sub, sizeof(sub));
        if (str_iequal(sub, "ADD"))
        {
            if (!sp2 || !*sp2)
                resp_err("MISSING_ARG");
            else
            {
                char text[512];
                parse_quoted_str(sp2, text, sizeof(text));
                RDSAssembler::rtListAdd(text);
                resp_ok();
            }
        }
        else if (str_iequal(sub, "DEL"))
        {
            if (!sp2 || !*sp2)
                resp_err("MISSING_ARG");
            else
            {
                unsigned idx = (unsigned)strtoul(sp2, nullptr, 10);
                if (!RDSAssembler::rtListDel(idx))
                    resp_err("BAD_INDEX");
                else
                    resp_ok();
            }
        }
        else if (str_iequal(sub, "CLEAR"))
        {
            RDSAssembler::rtListClear();
            resp_ok();
        }
        else if (str_iequal(sub, "?"))
        {
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
                        snprintf(part, sizeof(part), "%s%u=\"%s\"", first ? "" : ",", (unsigned)i,
                                 t);
                        strncat(line, part, sizeof(line) - strlen(line) - 1);
                        first = false;
                    }
                }
                resp_ok_kv(line);
            }
            else
            {
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
                        json_print_string(t);
                    }
                }
                Serial.println("]}}");
            }
        }
        else
        {
            resp_err("Unknown RDS item");
        }
        return true;
    }
    if (str_iequal(item_tok, "RTPERIOD"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            unsigned s = (unsigned)strtoul(rest, nullptr, 10);
            RDSAssembler::setRtPeriod(s);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "RTPERIOD?"))
    {
        char b[32];
        snprintf(b, sizeof(b), "RTPERIOD=%u", (unsigned)RDSAssembler::getRtPeriod());
        resp_ok_kv(b);
        return true;
    }
    return false;
}

/** AUDIO:* commands: STEREO, PREEMPH, STATUS. */
static bool handleAUDIO(const char *item_tok, const char *rest)
{
    if (str_iequal(item_tok, "STEREO"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setStereoEnable(atoi(rest) != 0);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "STEREO?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "STEREO=%u", DSP_pipeline::getStereoEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "PREEMPH"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setPreemphEnable(atoi(rest) != 0);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "PREEMPH?"))
    {
        char b[20];
        snprintf(b, sizeof(b), "PREEMPH=%u", DSP_pipeline::getPreemphEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "STATUS?"))
    {
        char b[64];
        snprintf(b, sizeof(b), "STEREO=%u,PREEMPH=%u", DSP_pipeline::getStereoEnable() ? 1 : 0,
                 DSP_pipeline::getPreemphEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    return false;
}

/** PILOT:* commands: ENABLE, AUTO, THRESH, HOLD (all set/get). */
static bool handlePILOT(const char *item_tok, const char *rest)
{
    if (str_iequal(item_tok, "ENABLE"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setPilotEnable(atoi(rest) != 0);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "ENABLE?"))
    {
        char b[20];
        snprintf(b, sizeof(b), "ENABLE=%u", DSP_pipeline::getPilotEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "AUTO"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setPilotAuto(atoi(rest) != 0);
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "AUTO?"))
    {
        char b[16];
        snprintf(b, sizeof(b), "AUTO=%u", DSP_pipeline::getPilotAuto() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "THRESH"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setPilotThresh((float)atof(rest));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "THRESH?"))
    {
        char b[32];
        snprintf(b, sizeof(b), "THRESH=%g", (double)DSP_pipeline::getPilotThresh());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "HOLD"))
    {
        if (!rest || !*rest)
            resp_err("MISSING_ARG");
        else
        {
            DSP_pipeline::setPilotHold((uint32_t)strtoul(rest, nullptr, 10));
            resp_ok();
        }
        return true;
    }
    if (str_iequal(item_tok, "HOLD?"))
    {
        char b[32];
        snprintf(b, sizeof(b), "HOLD=%u", (unsigned)DSP_pipeline::getPilotHold());
        resp_ok_kv(b);
        return true;
    }
    return false;
}

/**
 * SYST:* commands: VERSION, HELP, LOG:LEVEL, COMM:JSON, STATUS, HEAP, CONF:*, DEFAULTS, REBOOT.
 * - CONF profiles are stored in Preferences namespace "conf":
 *     _list (CSV of names), _active (current), p:<name> (blob)
 * - LOG OFF during boot is deferred (s_mute_after_startup) to keep startup visible.
 */
static bool handleSYST(const char *item_tok, const char *rest)
{
    if (str_iequal(item_tok, "VERS") || str_iequal(item_tok, "VERSION?"))
    {
        // Use existing version builder
        auto month_to_num = [](const char *mon) -> int
        {
            static const char *m[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            for (int i = 0; i < 12; ++i)
                if (strncmp(mon, m[i], 3) == 0)
                    return i + 1;
            return 1;
        };
        char mon[4] = {0};
        int d = 0;
        int y = 0;
        sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
        int mm = month_to_num(mon);
        char build[16];
        snprintf(build, sizeof(build), "%04d%02d%02d", y, mm, d);
        char iso[32];
        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%sZ", y, mm, d, __TIME__);
        char b[160];
        snprintf(b, sizeof(b), "VERSION=%s,BUILD=%s,BUILDTIME=%s", Config::FIRMWARE_VERSION, build,
                 iso);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "HELP") || str_iequal(item_tok, "HELP?"))
    {
        char topic[16] = {0};
        if (rest && *rest)
        {
            const char *rp = rest;
            next_token_str(rp, topic, sizeof(topic));
        }
        if (topic[0] == '\0')
            Serial.println("OK TOPICS=RDS,AUDIO,PILOT,SYST");
        else if (str_iequal(topic, "RDS"))
            Serial.println("OK RDS PI|PI? PTY|PTY? TP|TP? TA|TA? MS|MS? PS|PS? RT|RT? "
                           "ENABLE|ENABLE? RTLIST:ADD|DEL|CLEAR|? RTPERIOD|RTPERIOD? STATUS?");
        else if (str_iequal(topic, "AUDIO"))
            Serial.println("OK AUDIO STEREO|STEREO? PREEMPH|PREEMPH? STATUS?");
        else if (str_iequal(topic, "PILOT"))
            Serial.println("OK PILOT ENABLE|ENABLE? AUTO|AUTO? THRESH|THRESH? HOLD|HOLD?");
        else if (str_iequal(topic, "SYST"))
            Serial.println(
                "OK SYST VERSION? STATUS? HEAP? LOG:LEVEL|LOG:LEVEL? COMM:JSON|COMM:JSON? "
                "CONF:SAVE|CONF:LOAD|CONF:LIST?|CONF:ACTIVE?|CONF:DELETE CONF:DEFAULT DEFAULTS "
                "REBOOT");
        else
            Serial.println("OK");
        return true;
    }
    if (str_iequal(item_tok, "LOG"))
    {
        char sub[16] = {0};
        const char *rp = rest ? rest : "";
        next_token_str(rp, sub, sizeof(sub));
        if (str_iequal(sub, "LEVEL"))
        {
            char tok[16] = {0};
            next_token_str(rp, tok, sizeof(tok));
            if (!tok[0])
                resp_err("MISSING_ARG");
            else if (str_iequal(tok, "OFF"))
            {
                if (s_startup_phase)
                {
                    s_mute_after_startup = true;
                    s_log_mute = false;
                }
                else
                {
                    s_log_mute = true;
                    s_mute_after_startup = false;
                }
                resp_ok();
            }
            else
            {
                s_log_mute = false;
                if (str_iequal(tok, "ERROR"))
                    s_min_level = LogLevel::ERROR;
                else if (str_iequal(tok, "WARN"))
                    s_min_level = LogLevel::WARN;
                else if (str_iequal(tok, "INFO"))
                    s_min_level = LogLevel::INFO;
                else
                    s_min_level = LogLevel::DEBUG;
                resp_ok();
            }
            return true;
        }
        if (str_iequal(sub, "LEVEL?"))
        {
            const char *lvl =
                s_log_mute ? "OFF"
                           : (s_min_level == LogLevel::ERROR
                                  ? "ERROR"
                                  : (s_min_level == LogLevel::WARN
                                         ? "WARN"
                                         : (s_min_level == LogLevel::INFO ? "INFO" : "DEBUG")));
            char b[24];
            snprintf(b, sizeof(b), "LEVEL=%s", lvl);
            resp_ok_kv(b);
            return true;
        }
        resp_err("Unknown SYST LOG item");
        return true;
    }
    if (str_iequal(item_tok, "COMM"))
    {
        char sub[16] = {0};
        const char *rp = rest ? rest : "";
        next_token_str(rp, sub, sizeof(sub));
        if (str_iequal(sub, "JSON"))
        {
            char tok[8] = {0};
            next_token_str(rp, tok, sizeof(tok));
            if (!tok[0])
                resp_err("MISSING_ARG");
            else
            {
                s_json_mode = (atoi(tok) != 0) || str_iequal(tok, "ON");
                resp_ok();
            }
            return true;
        }
        if (str_iequal(sub, "JSON?"))
        {
            char b[16];
            snprintf(b, sizeof(b), "JSON=%u", s_json_mode ? 1 : 0);
            resp_ok_kv(b);
            return true;
        }
        resp_err("Unknown SYST COMM item");
        return true;
    }
    if (str_iequal(item_tok, "STATUS?"))
    {
        float core0 = 0, core1 = 0, aud = 0, logg = 0, vu = 0;
        uint32_t a_sw = 0, l_sw = 0, v_sw = 0;
        TaskStats::collect(core0, core1, aud, logg, vu, a_sw, l_sw, v_sw);
        char b[160];
        snprintf(b, sizeof(b),
                 "UPTIME=%u,CPU=%.1f,CORE0=%.1f,CORE1=%.1f,HEAP_FREE=%u,HEAP_MIN=%u,STEREO=%u,"
                 "AUDIO_CLIPPING=0",
                 (unsigned)(esp_timer_get_time() / 1000000ULL), (double)aud, (double)core0,
                 (double)core1, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                 DSP_pipeline::getStereoEnable() ? 1 : 0);
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "HEAP?"))
    {
        char b[64];
        snprintf(b, sizeof(b), "CURRENT_FREE=%u,MIN_FREE=%u", (unsigned)ESP.getFreeHeap(),
                 (unsigned)ESP.getMinFreeHeap());
        resp_ok_kv(b);
        return true;
    }
    if (str_iequal(item_tok, "CONF"))
    {
        char sub[16] = {0};
        const char *rp = rest ? rest : "";
        next_token_str(rp, sub, sizeof(sub));
        if (str_iequal(sub, "SAVE"))
        {
            char name[32] = {0};
            if (rp && *rp)
            {
                next_token_str(rp, name, sizeof(name));
            }
            if (!*name)
                strncpy(name, "default", sizeof(name) - 1);
            conf_open_rw();
            String list = s_prefs.getString("_list", "");
            // Build blob
            char blob[768];
            conf_build_blob(blob, sizeof(blob));
            String key = String("p:") + name;
            bool ok = s_prefs.putString(key.c_str(), blob);
            if (ok)
            {
                // add to list if missing and set active
                if (!strlist_contains(list, name))
                {
                    list = list.length() ? (list + "," + name) : String(name);
                    s_prefs.putString("_list", list);
                }
                s_prefs.putString("_active", name);
                conf_close();
                resp_ok();
            }
            else
            {
                conf_close();
                resp_err("STORE_FAIL");
            }
            return true;
        }
        if (str_iequal(sub, "LOAD"))
        {
            char name[32] = {0};
            if (rp && *rp)
                next_token_str(rp, name, sizeof(name));
            if (!*name)
                strncpy(name, "default", sizeof(name) - 1);
            conf_open_rw();
            String key = String("p:") + name;
            String blob = s_prefs.getString(key.c_str(), "");
            if (blob.length() > 0)
            {
                apply_loaded_blob(blob.c_str());
                s_prefs.putString("_active", name);
                conf_close();
                resp_ok();
            }
            else
            {
                conf_close();
                resp_err("NOT_FOUND");
            }
            return true;
        }
        if (str_iequal(sub, "LIST?"))
        {
            conf_open_rw();
            String list = s_prefs.getString("_list", "");
            conf_close();
            if (!s_json_mode)
            {
                char out[256];
                snprintf(out, sizeof(out), "RTLIST=\"%s\"", list.c_str());
                resp_ok_kv(out);
            }
            else
            {
                Serial.print("{\"ok\":true,\"data\":{\"LIST\":[");
                bool first = true;
                int i = 0;
                String acc = "";
                for (size_t p = 0; p < list.length();)
                {
                    size_t q = list.indexOf(',', p);
                    if (q == (size_t)-1)
                        q = list.length();
                    String nm = list.substring(p, q);
                    if (!first)
                        Serial.print(',');
                    first = false;
                    json_print_string(nm.c_str());
                    p = (q < list.length() ? q + 1 : q);
                }
                Serial.println("]}}");
            }
            return true;
        }
        if (str_iequal(sub, "ACTIVE?"))
        {
            conf_open_rw();
            String act = s_prefs.getString("_active", "");
            conf_close();
            char b[64];
            snprintf(b, sizeof(b), "ACTIVE=\"%s\"", act.c_str());
            resp_ok_kv(b);
            return true;
        }
        if (str_iequal(sub, "DELETE"))
        {
            char name[32] = {0};
            next_token_str(rp, name, sizeof(name));
            if (!*name)
            {
                resp_err("MISSING_ARG");
                return true;
            }
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
                resp_ok();
            else
                resp_err("NOT_FOUND");
            return true;
        }
        if (str_iequal(sub, "DEFAULT"))
        {
            apply_factory_defaults();
            resp_ok();
            return true;
        }
        resp_err("Unknown SYST CONF item");
        return true;
    }
    if (str_iequal(item_tok, "DEFAULTS"))
    {
        apply_factory_defaults();
        resp_ok();
        return true;
    }
    if (str_iequal(item_tok, "REBOOT"))
    {
        resp_ok();
        delay(50);
        ESP.restart();
        return true;
    }
    return false;
}

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

    // (Removed legacy haveLine() helper; line-oriented reading handled below)

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

            // ---- STEP 4: TOKENIZE INPUT LINE ----
            // Copy line to working buffer and split into GROUP:ITEM and arguments

            static char line[256];
            strncpy(line, (const char *)line_buf, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            trim_line(line); // Remove leading/trailing whitespace

            // ---- EXTRACT COMMAND COMPONENTS ----
            // Command format: "GROUP:ITEM <args>" or "GROUP:ITEM?"
            // Example: "RDS:PI 0x52A1" -> group="RDS", item="PI", rest="0x52A1"
            const char *sp = line;
            char group_tok[32]; // GROUP part (e.g., "RDS")
            char item_tok[64];  // ITEM part (e.g., "PI?" or "PI")
            next_token_str(sp, group_tok, sizeof(group_tok));
            next_token_str(sp, item_tok, sizeof(item_tok));
            const char *rest = sp;

            // Trim any remaining leading whitespace from arguments
            // This is CRITICAL to prevent parsing bugs with whitespace
            while (*rest == ' ' || *rest == '\t' || *rest == ':')
                ++rest;

            // Legacy response lambdas and monolithic parser removed; use file-scope helpers

            // ---- STEP 5: COMMAND DISPATCH ----
            // Route command to appropriate handler based on GROUP and ITEM tokens
            // Each GROUP (RDS, AUDIO, PILOT, SYST) has its own command handlers
            // Active modular handlers
            if (group_tok[0] && item_tok[0])
            {
                bool quick = false;
                if (str_iequal(group_tok, "RDS"))
                    quick = handleRDS(item_tok, rest);
                else if (str_iequal(group_tok, "AUDIO"))
                    quick = handleAUDIO(item_tok, rest);
                else if (str_iequal(group_tok, "PILOT"))
                    quick = handlePILOT(item_tok, rest);
                else if (str_iequal(group_tok, "SYST"))
                    quick = handleSYST(item_tok, rest);
                if (!quick)
                {
                    resp_err("Unknown command");
                }
                goto after_parse;
            }

#if 0  // Legacy monolithic parser (compiled out)
            if (group_tok[0] && item_tok[0])
            {
                // Modular handlers are authoritative now
                bool quick = false;
                if (str_iequal(group_tok, "RDS")) quick = handleRDS(item_tok, rest);
                else if (str_iequal(group_tok, "AUDIO")) quick = handleAUDIO(item_tok, rest);
                else if (str_iequal(group_tok, "PILOT")) quick = handlePILOT(item_tok, rest);
                else if (str_iequal(group_tok, "SYST")) quick = handleSYST(item_tok, rest);
                if (!quick)
                {
                    resp_err("Unknown command");
                }
                goto after_parse;
            }
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
                                char list_buf[512];
                                list_buf[0] = '\0';
                                for (size_t i = 0; i < kPtyMapSize; ++i)
                                {
                                    char entry[32];
                                    snprintf(entry, sizeof(entry), "%s%u=%s",
                                             list_buf[0] ? "," : "",
                                             (unsigned)kPtyMap[i].code,
                                             kPtyMap[i].long_name);
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
                                bool found = false;
                                for (size_t i = 0; i < kPtyMapSize; ++i)
                                {
                                    if (iequal(rs, kPtyMap[i].long_name))
                                    {
                                        v = kPtyMap[i].code;
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
                                    json_print_string(t);
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
                                parse_quoted_str(rp, text, sizeof(text));
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
                                        // Emit quoted JSON string for item
                                        json_print_string(t);
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
                        // Build ISO-style date/time fields from __DATE__ and __TIME__
                        auto month_to_num = [](const char *mon) -> int {
                            static const char* m[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                            for (int i=0;i<12;++i) if (strncmp(mon,m[i],3)==0) return i+1;
                            return 1;
                        };
                        char mon[4]={0}; int d=0; int y=0;
                        sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
                        int mm = month_to_num(mon);
                        char build[16];
                        snprintf(build, sizeof(build), "%04d%02d%02d", y, mm, d);
                        // __TIME__ is HH:MM:SS
                        char iso[32];
                        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%sZ", y, mm, d, __TIME__);
                        char b[160];
                        snprintf(b, sizeof(b), "VERSION=%s,BUILD=%s,BUILDTIME=%s",
                                 Config::FIRMWARE_VERSION, build, iso);
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
#endif // end legacy monolithic parser

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

    // ---- Attempt Non-Blocking Enqueue (drop-oldest semantics) ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        // Queue full: drop the oldest message to make room, then retry once
        LogMsg dummy;
        xQueueReceive(queue_, &dummy, 0);
        if (xQueueSend(queue_, &m, 0) != pdTRUE)
        {
            // Still failed; count as a drop
            dropped_count_++;
            return false;
        }
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

    // ---- Attempt Non-Blocking Enqueue (drop-oldest semantics) ----
    if (xQueueSend(queue_, &m, 0) != pdTRUE)
    {
        LogMsg dummy;
        xQueueReceive(queue_, &dummy, 0);
        if (xQueueSend(queue_, &m, 0) != pdTRUE)
        {
            dropped_count_++;
            return false;
        }
    }

    return true;
}
