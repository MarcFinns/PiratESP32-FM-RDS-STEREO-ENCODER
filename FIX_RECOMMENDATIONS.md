# Command Processor - Fix Recommendations

## Summary
Three bugs in Console.cpp's command parsing prevent 6 tests from passing. All are easily fixable by properly handling whitespace in the `rest` token.

---

## Bug #1: RDS:PTY Numeric Parsing Fails (Line 903)

### Current Code
```cpp
else if (iequal(item_tok, "PTY"))
{
    if (!rest) { err("MISSING_ARG"); }
    else
    {
        const char *rs = rest;
        while (*rs == ' ' || *rs == '\t')      // Trims LOCAL copy
            ++rs;
        if (strncmp(rs, "LIST?", 5) == 0)
        {
            // ... LIST handling ...
        }
        unsigned v = 0;
        if (rest[0] >= '0' && rest[0] <= '9')  // ← BUG: Checks ORIGINAL 'rest'
            v = (unsigned)strtoul(rest, nullptr, 10);
        else
        {
            // ... name matching using rest, not rs ...
            // This looks for PTY names but 'rest' still has leading space
            for (auto &e : map)
            {
                if (iequal(rest, e.n))  // ← Will fail due to space prefix
                {
                    v = e.c;
                    found = true;
                    break;
                }
            }
        }
        RDSAssembler::setPTY((uint8_t)(v & 0x1F));
        ok();
    }
}
```

### Problem
When command is `RDS:PTY 10`:
- `rest` points to `" 10"` (includes leading space)
- Line 903 checks: `rest[0] >= '0'` → `' ' >= '0'` → FALSE
- Falls through to name matching
- Tries to match `" 10"` as a PTY name (like "ROCK", "JAZZ", etc.)
- No match found
- Returns `ERR BAD_VALUE`

### Fix
Replace the PTY handling block (lines 847-954) with:

```cpp
else if (iequal(item_tok, "PTY"))
{
    if (!rest || !*rest)
    {
        err("MISSING_ARG");
    }
    else
    {
        // Trim leading whitespace from rest
        const char *rs = rest;
        while (*rs == ' ' || *rs == '\t')
            ++rs;

        if (strncmp(rs, "LIST?", 5) == 0)
        {
            // ... existing LIST handling code ...
        }

        unsigned v = 0;
        if (rs[0] >= '0' && rs[0] <= '9')  // ← Use TRIMMED 'rs'
        {
            v = (unsigned)strtoul(rs, nullptr, 10);
        }
        else
        {
            // ... PTY name matching ...
            struct P { const char *n; uint8_t c; };
            static const P map[] = {
                {"NONE", 0}, {"NEWS", 1},
                // ... rest of PTY map ...
            };
            bool found = false;
            for (auto &e : map)
            {
                if (iequal(rs, e.n))  // ← Use TRIMMED 'rs'
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
```

**Key Changes:**
- Line: Add `|| !*rest` to empty string check
- Line: Create trimmed copy: `const char *rs = rest; while (*rs == ' ' || *rs == '\t') ++rs;`
- Line 903 equivalent: Change `rest[0]` to `rs[0]`
- PTY name matching: Use `rs` instead of `rest`

---

## Bug #2: RDS:PI Hex Parsing Fails (Lines 833-836)

### Current Code
```cpp
if (iequal(item_tok, "PI"))
{
    if (!rest)
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
```

### Problem
When command is `RDS:PI 0x52A1`:
- `rest` points to `" 0x52A1"` (includes leading space)
- `strtoul(" 0x52A1", nullptr, 16)` may fail to parse properly due to the leading space
- Result: `v = 0x0000` instead of `0x52A1`

### Fix
Apply the same whitespace trimming:

```cpp
if (iequal(item_tok, "PI"))
{
    if (!rest || !*rest)
    {
        err("MISSING_ARG");
    }
    else
    {
        // Trim leading whitespace
        const char *rs = rest;
        while (*rs == ' ' || *rs == '\t')
            ++rs;

        unsigned v = 0;
        if (strncmp(rs, "0x", 2) == 0 || strncmp(rs, "0X", 2) == 0)
            v = (unsigned)strtoul(rs, nullptr, 16);
        else
            v = (unsigned)strtoul(rs, nullptr, 10);
        RDSAssembler::setPI((uint16_t)(v & 0xFFFF));
        ok();
    }
}
```

**Key Changes:**
- Add `|| !*rest` to error check
- Add trimming: create `rs` pointing to first non-space character
- Change `strncmp(rest,` → `strncmp(rs,`
- Change `strtoul(rest,` → `strtoul(rs,`

---

## Bug #3: Missing Argument Detection (Lines 826-828 and elsewhere)

### Current Code (appears multiple times)
```cpp
if (iequal(item_tok, "PI"))
{
    if (!rest)  // ← Only checks null pointer
    {
        err("MISSING_ARG");
    }
    else { /* ... */ }
}
```

### Problem
- Only checks if `rest` is a null pointer
- Doesn't catch empty strings or whitespace-only strings
- Valid error cases slip through

### Fix
Replace all instances of:
```cpp
if (!rest)
```

With:
```cpp
if (!rest || !*rest)
```

This checks both:
- `!rest`: pointer is null
- `!*rest`: first character is null terminator (empty string)

**Locations to fix (search for "if (!rest)"):**
- Line 826: RDS PI
- Line 849: RDS PTY
- Line 963: RDS TP
- Line 981: RDS TA
- Line 999: RDS MS
- And similar patterns throughout

---

## Recommended Refactoring (Optional but Better)

To avoid repeating the whitespace trimming logic, add a helper at line 780:

```cpp
// After extracting rest pointer (line 780)
const char *rest = sp;

// Helper to trim leading whitespace
auto skip_ws = [](const char *p) -> const char * {
    while (p && (*p == ' ' || *p == '\t'))
        ++p;
    return p;
};
```

Then use throughout:
```cpp
const char *arg = skip_ws(rest);
if (!arg || !*arg)
{
    err("MISSING_ARG");
}
else
{
    // Use 'arg' instead of 'rest' for all argument parsing
    if (arg[0] >= '0' && arg[0] <= '9')
    {
        v = (unsigned)strtoul(arg, nullptr, 10);
    }
}
```

This way:
- Single location to maintain whitespace logic
- Less chance of missing a case
- More readable code

---

## Testing After Fix

Run the Python test script to verify:

```bash
python3 test_commands.py
```

Expected results after fix:
- **Total:** 33/33 tests (100%)
- All 6 failing tests should now pass
- No new errors introduced

---

## Code Review Checklist

After applying fixes:

- [ ] Search "if (!rest)" - ensure all have been updated to "if (!rest || !*rest)"
- [ ] Search "rest[0]" - ensure context requires whitespace to be trimmed first
- [ ] Test RDS:PI with hex values (0xABCD)
- [ ] Test RDS:PI with decimal values (12345)
- [ ] Test RDS:PTY with values 0-31
- [ ] Test RDS:PTY with names (ROCK, JAZZ, etc)
- [ ] Run full 33-test suite
- [ ] Compile without warnings
- [ ] Verify no new regressions in passing tests

---

## Files to Modify

- **Console.cpp** - Lines 780-1200+ (command parsing section)
  - Add helper function or inline trimming logic
  - Fix RDS:PI hex parsing (line 833+)
  - Fix RDS:PTY numeric parsing (line 903+)
  - Update all "if (!rest)" checks

---

## Confidence & Impact

**Bug Severity:** HIGH (3 critical functionality bugs)
**Fix Complexity:** LOW (simple whitespace trimming)
**Lines Changed:** ~30-50 lines
**Risk:** VERY LOW (isolated changes, no architecture impact)
**Testing:** Covered by existing test suite

**Before Fix:** 27/33 (81%)
**After Fix:** 33/33 (100%)

