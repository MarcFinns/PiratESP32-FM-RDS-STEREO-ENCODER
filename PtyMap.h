// Simple PTY mapping shared by Console and Display
#pragma once
#include <cstdint>
#include <cstddef>

struct PtyEntry {
    uint8_t code;
    const char* long_name;  // SCPI-facing name
    const char* short_label; // UI label
};

inline constexpr PtyEntry kPtyMap[] = {
    {0,  "NONE",             "NONE"},
    {1,  "NEWS",             "NEWS"},
    {2,  "INFORMATION",      "INFO"},
    {3,  "SPORT",            "SPORT"},
    {4,  "TALK",             "TALK"},
    {5,  "ROCK",             "ROCK"},
    {6,  "CLASSIC_ROCK",     "CROCK"},
    {7,  "ADULT_HITS",       "HITS"},
    {8,  "SOFT_ROCK",        "SROCK"},
    {10, "TOP_40",           "TOP40"},
    {11, "COUNTRY",          "CNTRY"},
    {13, "OLDIES",           "OLDIES"},
    {14, "SOFT",             "SOFT"},
    {15, "JAZZ",             "JAZZ"},
    {16, "CLASSICAL",        "CLASS"},
    {17, "RNB",              "RNB"},
    {18, "SOFT_RNB",         "SRNB"},
    {19, "LANGUAGE",         "LANG"},
    {20, "RELIGIOUS_MUSIC",  "RELM"},
    {21, "RELIGIOUS_TALK",   "RELT"},
    {22, "PERSONALITY",      "PERS"},
    {24, "PUBLIC",           "PUBLIC"},
    {27, "COLLEGE",          "COLL"},
};

inline constexpr size_t kPtyMapSize = sizeof(kPtyMap) / sizeof(kPtyMap[0]);

inline const PtyEntry* findPtyByCode(uint8_t code) {
    for (size_t i=0;i<kPtyMapSize;++i) if (kPtyMap[i].code==code) return &kPtyMap[i];
    return nullptr;
}

inline bool eq_upper(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a>='a'&&*a<='z')?(*a-32):*a;
        char cb = (*b>='a'&&*b<='z')?(*b-32):*b;
        if (ca!=cb) return false; ++a; ++b;
    }
    return *a==0 && *b==0;
}

inline const PtyEntry* findPtyByLong(const char* nameUpper) {
    for (size_t i=0;i<kPtyMapSize;++i) if (eq_upper(kPtyMap[i].long_name, nameUpper)) return &kPtyMap[i];
    return nullptr;
}

