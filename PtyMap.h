/*
 * =====================================================================================
 *
 *                      PiratESP32 - FM RDS STEREO ENCODER
 *                      (c) 2025 MFINI, Anthropic Claude Code, OpenAI Codex
 *                         RDS PTY Mapping (EU Standard)
 *
 * =====================================================================================
 *
 * File:         PtyMap.h
 * Description:  Programme Type (PTY) mapping for European RDS (EN 50067)
 *
 * Purpose:
 *   Provides a single source of truth for PTY codes used across the firmware:
 *   - Console (SCPI): Uses long_name tokens (UPPER_SNAKE) for RDS:PTY set/list
 *   - Display UI: Uses short_label tokens (4–6 chars) for compact on-screen chips
 *
 * Notes:
 *   - This table implements the European RDS PTY list (0–31), not US RBDS.
 *   - Long names avoid spaces and use underscores for unambiguous SCPI tokens.
 *   - Short labels are optimized for a narrow status bar; adjust to taste.
 *
 * =====================================================================================
 */

#pragma once

#include <cstdint>
#include <cstddef>

struct PtyEntry {
    uint8_t code;           // 0..31
    const char* long_name;  // SCPI-facing name (e.g., "CURRENT_AFFAIRS")
    const char* short_label;// UI label (e.g., "AFFRS")
};

// European RDS PTY codes (EN 50067 / IEC 62106-3)
inline constexpr PtyEntry kPtyMap[] = {
    { 0, "NONE",              "NONE" },
    { 1, "NEWS",              "NEWS" },
    { 2, "CURRENT_AFFAIRS",   "AFFRS" },
    { 3, "INFORMATION",       "INFO" },
    { 4, "SPORT",             "SPORT" },
    { 5, "EDUCATION",         "EDU" },
    { 6, "DRAMA",             "DRAMA" },
    { 7, "CULTURE",           "CULT" },
    { 8, "SCIENCE",           "SCI" },
    { 9, "VARIED",            "VAR" },
    {10, "POP_MUSIC",         "POP" },
    {11, "ROCK_MUSIC",        "ROCK" },
    {12, "EASY_LISTENING",    "EASY" },
    {13, "LIGHT_CLASSICAL",   "LCLAS" },
    {14, "SERIOUS_CLASSICAL", "SCLAS" },
    {15, "OTHER_MUSIC",       "OTHER" },
    {16, "WEATHER",           "WETHR" },
    {17, "FINANCE",           "FIN" },
    {18, "CHILDREN",          "KIDS" },
    {19, "SOCIAL_AFFAIRS",    "SOC" },
    {20, "RELIGION",          "REL" },
    {21, "PHONE_IN",          "PHONE" },
    {22, "TRAVEL",            "TRAVL" },
    {23, "LEISURE",           "LEIS" },
    {24, "JAZZ_MUSIC",        "JAZZ" },
    {25, "COUNTRY_MUSIC",     "CNTRY" },
    {26, "NATIONAL_MUSIC",    "NAT" },
    {27, "OLDIES_MUSIC",      "OLDIES" },
    {28, "FOLK_MUSIC",        "FOLK" },
    {29, "DOCUMENTARY",       "DOC" },
    {30, "ALARM_TEST",        "ALTEST" },
    {31, "ALARM",             "ALARM" },
};

inline constexpr size_t kPtyMapSize = sizeof(kPtyMap) / sizeof(kPtyMap[0]);

inline const PtyEntry* findPtyByCode(uint8_t code)
{
    for (size_t i = 0; i < kPtyMapSize; ++i)
        if (kPtyMap[i].code == code)
            return &kPtyMap[i];
    return nullptr;
}

inline bool eq_upper(const char* a, const char* b)
{
    while (*a && *b)
    {
        char ca = (*a >= 'a' && *a <= 'z') ? (*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (*b - 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

inline const PtyEntry* findPtyByLong(const char* nameUpper)
{
    for (size_t i = 0; i < kPtyMapSize; ++i)
        if (eq_upper(kPtyMap[i].long_name, nameUpper))
            return &kPtyMap[i];
    return nullptr;
}
