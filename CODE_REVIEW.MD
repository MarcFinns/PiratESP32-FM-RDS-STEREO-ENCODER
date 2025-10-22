Code Review — PiratESP32 FM MPX Stereo RDS Encoder

Summary
- Overall architecture is solid: four-task design (DSP, Console, RDSAssembler, DisplayManager) cleanly separates real‑time audio from I/O and UI.
- Console centralizes Serial ownership and logging; non‑blocking queues protect the audio path; NVS profiles are a good addition.
- RDSAssembler now builds real 0A/2A groups with CRC/offsets and a simple scheduler; Display marquee logic is decoupled from the broadcast 64‑char RT.
- Most regressions seen earlier (PS corruption, splash timing, status bar boot loop) appear addressed, but a few inconsistencies, duplications, and cleanup opportunities remain.
- This document calls out concrete issues with file/line pointers and proposes a staged intervention plan focused on correctness, maintainability, and predictability.

Architectural Soundness
- Task model (good):
  - `DSP_pipeline` on Core 0 at high priority; `Console`, `RDSAssembler`, and `DisplayManager` on Core 1 at lower priorities. Matches real‑time constraints and avoids Serial contention.
  - Queues: mailbox (overwrite) for VU samples and stats; FIFO for RDS bits with drop‑oldest; Console message queue for logs.
- Serial ownership (good):
  - Console owns RX/TX and drains a queue of formatted logs. Command parser and logging filter live in one place.
- RDS broadcast vs UI (good):
  - Broadcast RT (64 chars, AB toggle) is set by RDSAssembler; Display builds a long marquee from RTLIST with delimiter and swap‑on‑wrap behavior.
- Splash + system startup (good):
  - Startup speech is serialized through Console, then muted if requested; splash runs before tasks start.
- Notable risks:
  - Global runtime flags in `DSP_pipeline` are updated from Console on a different core without explicit barriers. They’re 32‑bit or smaller and volatile, so this is practically safe on ESP32, but consider adding brief comments on memory ordering to avoid future misunderstandings (e.g., “single‑word atomic writes on Xtensa; no compound invariants”).
  - Console queue capacity and stack are generous; memory pressure may vary across boards/sketches.

Architectural Consistency / Patterns
- Console queue “drop‑oldest” vs implementation (inconsistent):
  - Docs claim drop‑oldest on overflow for Console logs (QueueContracts.md), but code drops the new message.
    - Docs: `QueueContracts.md:16` (Console=Drop oldest)
    - Code: `Console.cpp:2134` uses `xQueueSend(..., 0)` and on failure increments `dropped_count_`, i.e., drops new.
  - Suggest either (a) implement drop‑oldest like RDSAssembler (pop one when full then send), or (b) adjust docs to “drop‑new” for Console.
- SYST:VERSION? payload format (inconsistent with docs):
  - Docs show `BUILD=YYYYMMDD, BUILDTIME=YYYY-MM-DDThh:mm:ssZ`.
  - Code: `Console.cpp:1724` returns `BUILD=%s` with `__DATE__` and `BUILDTIME=%s %s` (non‑ISO, trailing space artifact).
- PTY names (inconsistent across modules):
  - Console uses long RBDS-like names for set (e.g., `CLASSIC_ROCK`, `SOFT_RNB`). `Console.cpp:1159`.
  - Display shows abbreviated tokens (e.g., `CROCK`, `SROCK`). `DisplayManager.cpp:585+`.
  - That’s acceptable for UI, but keep a single source of truth for numeric↔name mapping and derive display labels from it.
- JSON replies (hand‑rolled in multiple places):
  - Escaping logic is duplicated (`Console.cpp:940–1110` and `:1888–2012`). Consider centralizing JSON emission helpers to avoid drift.

Code Duplications
- PTY mapping tables duplicated:
  - `Console.cpp:1148–1185` and `DisplayManager.cpp:585–626`. Consolidate in a small header (e.g., `PtyMap.h`) that provides both long and short labels.
- UI layout constants duplicated:
  - Some constants are defined both in `DisplayManager::begin()` and in `::process()` (e.g., `MARGIN_X`, VU widths, divider positions). See `DisplayManager.cpp:213–238` vs `:302–337` and `:287–296`. Move to internal `constexpr` or `Config.h` UI block to avoid mismatches.
- RTLIST JSON/text printing code appears twice:
  - In `RDS:RTLIST?` and in `RDS:RTLIST ?` subcommand branch (`Console.cpp:1344–1391` and `:1491–1561`). Refactor to one helper.
- JSON escaping logic appears 3+ times:
  - `Console.cpp:955–1110`, `:1888–2012`, and small inlined loops elsewhere. Extract minimal `emit_json_string(const char*)`.

Dead / Unused Code
- Legacy `haveLine()` lambda in Console (unused):
  - `Console.cpp:742–772` — Comment says “unused legacy code”. Safe to remove.
- `DisplayManager::setDisplayRT()` not referenced by current code path:
  - Present for future external feeds; if not needed, remove or add a single call site (e.g., accept a UI override via SCPI) to justify keeping the API.
- Disabled stats panel draw:
  - Guarded with `if (false && stats_queue_)` in `DisplayManager.cpp:1020`. Either wire to a config flag (already exists: `Config::DISPLAY_SHOW_RDS_STATUS_BAR`) or remove the dead block.

Debug / Flags To Clean Up
- README mentions `LEVEL_TAPS_ENABLE` which does not exist anymore:
  - `README.md:152` vs no definition in `Config.h`. Update docs or re‑introduce a guarded diagnostic block if still desired.
- Performance logging “always enabled” comment in `Config.h` conflicts with runtime muting behavior:
  - Header comment: `Config.h:27–35` vs actual gating in Console drain (mute after startup). Align commentary to reflect current behavior.
- Console queue size/stack sizing:
  - `Config.h:232–238`: `CONSOLE_STACK_WORDS=8192` (32 KB) and `CONSOLE_QUEUE_LEN=256` (≈ 43 KB payload). Consider trimming after startup or making queue len conditional to reduce RAM pressure.

Comments / Docs Not In Sync with Code
- RDSAssembler summary now correctly advertises 0A/2A only (no CT), which matches implementation.
- Console queue behavior docs (drop‑oldest) vs code (drop‑new) — see above.
- `SYST:VERSION?` formatting mismatch — see above.
- README config values differ from `Config.h` (e.g., `DIFF_AMP`/`PILOT_AMP`, and `LEVEL_TAPS_ENABLE` mention). Keep README in lockstep with `Config.h` defaults.

Reliability & Performance Notes
- Logging mute after startup works by filtering at drain time:
  - Pros: minimal code change; preserves enqueue call sites.
  - Cons: logs still enqueue periodically; they’re just dropped during drain. Not heavy (5s cadence), but you can further reduce overhead by not enqueuing performance logs when `s_log_mute` is ON (requires making the mute state visible to DSP, or adding a cheap “shouldLog()” helper).
- Stack usage in Console:
  - Many buffers are static already, but parser complexity means tight margins. The current 32 KB stack is safe; still, removing the unused `haveLine()` and extracting small helpers reduces stack pressure.
- Display stability:
  - Status bar drawing is guarded by `Config::DISPLAY_SHOW_RDS_STATUS_BAR` and a 1.5 s initial deferral (`DisplayManager.cpp:550–569`), which mitigated earlier boot loops. If further stability issues arise, add an upper bound on characters drawn per frame or throttle bar updates to 1 Hz.
- Concurrency:
  - Runtime toggles in `DSP_pipeline` are `volatile` scalars. Reads/writes are single instruction on ESP32 Xtensa; acceptable without locks. Document this in code to prevent accidental expansion into multi‑word structs later without synchronization.

Specific Findings (Selected)
- Console parser size and cohesion:
  - `Console.cpp:730–2160` — `process()` handles draining, parsing, response building, NVS profiles, and JSON. Consider splitting into:
    1) tokenization + normalization, 2) dispatcher, 3) group handlers, 4) reply helpers.
- NVS profiles:
  - Keys `_list` and `_active` plus `p:<name>` blob: simple and effective (`Console.cpp:1826–2065`). Ensure max sizes cannot overflow (blob fits in Arduino Preferences limits; current concat approach is bounded by local buffer lengths).
- PS handling:
  - `RDSAssembler::setPS()` is now safe (bounded copy and pad), resolving earlier garbage characters (`RDSAssembler.cpp:124–145`).
- Marquee seam and spacing:
  - Correctly adds the delimiter between end and beginning for 2+ items, and a space gap for single RT fallback (`DisplayManager.cpp:848–901`).

Proposed Intervention Plan

Phase 1 — Hygiene (no functional change)
- Remove unused code:
  - Delete `haveLine()` (Console.cpp:742–772) and adjacent comment.
- Centralize constants:
  - Extract display layout constants used in both `begin()` and `process()` into a single internal `constexpr` block, or a small header inside `DisplayManager`.
- Docs alignment:
  - Update `README.md` config snippets to match `Config.h` (remove `LEVEL_TAPS_ENABLE`, correct amplitudes).
  - Update `docs/SerialConsole.md` examples for `SYST:VERSION?` to reflect actual fields (or change code; see Phase 2).
- Naming consistency:
  - In `TaskStats` comments and variable names, consider renaming “logger”→“console” to match the class rename (keep backward compatibility in task name detection, i.e., treat both names as Console, already handled: `TaskStats.cpp:134–144, 138–145`).

Phase 2 — Targeted Improvements (low risk)
- Console queue overflow semantics:
  - Implement true drop‑oldest on overflow to match docs:
    - On failed `xQueueSend`, do one `xQueueReceive(...,0)` then retry `xQueueSend` (mirrors the RDSAssembler pattern). Change `Console.cpp:2132–2142`.
- SYST:VERSION? format:
  - Emit `BUILD=YYYYMMDD` and `BUILDTIME=YYYY-MM-DDThh:mm:ssZ` while keeping `VERSION=Config::FIRMWARE_VERSION` (`Console.cpp:1718–1726`). This aligns with docs and makes parsing easier.
- JSON helper:
  - Extract a tiny `emit_json_string(const char*)` and reuse in both RTLIST JSON emitters and `ok_kv` to reduce duplication and risk of escaping bugs.
- PTY mapping:
  - Create `src/PtyMap.h` with a const array of `{code,long_name,short_label}` and use it in Console and Display for consistency. This also lets you implement `RDS:PTY LIST?` from one source.
- Logging overhead while OFF:
  - Add an inline `Console_ShouldLog()` query (reads s_min_level/s_log_mute) that modules can check before formatting noisy periodic logs. Start by using it in `DSP_pipeline::printPerformance()`.

Phase 3 — Modularization (optional, moderate scope)
- Console command processor:
  - Split `Console::process()` into a small dispatcher with per‑group handlers in separate static functions or a tiny registry `{group,item}->handler`. Improves readability and testability.
- RDSAssembler rotation list:
  - Move `s_rt_list`, `s_rt_index`, `s_rt_period_s`, and `s_rt_next_switch_us` into RDSAssembler as private members to avoid hidden global state and ease future multi‑instance testing.
- ConfigStore abstraction:
  - Wrap NVS get/put blob logic with a typed struct and (de)serializer for PI/PTY/PS/RT/flags. Centralizes defaults, validates sizes, and reduces string parsing bugs.

Suggested Test Additions
- Expand `tools/console_tester.py` coverage:
  - Verify SYST:CONF SAVE/LOAD/LIST/ACTIVE/DELETE cycles with empty RTLIST vs populated and with RT fallback.
  - Exercise JSON mode for `RDS:STATUS?`, `AUDIO:STATUS?`, and `SYST:STATUS?` with parsing.
  - Confirm `SYST:LOG:LEVEL OFF` keeps startup logs visible and suppresses periodic logs after the startup marker.
- Add a very small unit harness (if desired) for the RT marquee builder using a host build, or keep logic in a separate function for testability.

Quick Wins (Top 10)
1) Implement drop‑oldest in Console queue or fix the docs to match actual drop‑new behavior.
2) Fix `SYST:VERSION?` format or adjust docs; prefer fixing code for stable automation.
3) Remove `haveLine()` dead code in Console.
4) Centralize UI layout constants (avoid future mis‑refs like `TEXT_AREA_X`).
5) Extract JSON string emitter and reuse across handlers.
6) Consolidate PTY mapping in one header; derive both full and short labels from it.
7) Consider shrinking `CONSOLE_QUEUE_LEN` to 128 after startup (or keep at 256 but document memory footprint explicitly).
8) Align README config examples to `Config.h` (remove `LEVEL_TAPS_ENABLE`).
9) Document atomicity assumptions for runtime flags in `DSP_pipeline` and avoid future multi‑word state without guards.
10) Optionally add `Console_ShouldLog()` and call it before enqueuing periodic performance logs from DSP.

File References (pointers mentioned above)
- Console
  - Console queue drop behavior: `Console.cpp:2132` (enqueueRaw), `Console.cpp:2164` (enqueueFormatted)
  - Parser body and helpers: `Console.cpp:730` (process)
  - `SYST:VERSION?` emission: `Console.cpp:1718`
  - JSON building (escape): `Console.cpp:945`, `Console.cpp:1888`
  - RTLIST text/JSON emit: `Console.cpp:1344`, `Console.cpp:1491`
- RDSAssembler
  - PS/RT setters (fixed copy/pad): `RDSAssembler.cpp:124`, `RDSAssembler.cpp:142`
  - Group builders and bit queue (drop‑oldest): `RDSAssembler.cpp:338`, `RDSAssembler.cpp:267`
  - Rotation list state (globals right now): `RDSAssembler.cpp:48`–`:59`
- DisplayManager
  - Status bar draw and gating: `DisplayManager.cpp:550`–`:710`
  - PS/RT layout positions: `DisplayManager.cpp:707`–`:736`
  - Marquee builder and seam: `DisplayManager.cpp:820`–`:901`, `:1018`–`:1068`
- DSP_pipeline
  - Performance logging cadence: `DSP_pipeline.cpp:427`–`:451`, `:832`–`:912` (printPerformance)
  - Pilot auto‑mute runtime flags: `DSP_pipeline.cpp:582`–`:614`
- Docs
  - Console queue contract: `QueueContracts.md:14–46`
  - SerialConsole spec: `docs/SerialConsole.md`
  - README config block: `README.md:140–180`

Closing
The project is in good shape and already implements the requested behavior (single Serial owner, long RT marquee, pilot auto‑mute on silence). The recommended changes are mostly hygiene and consistency fixes that will reduce future regressions and make the code base easier to extend. I’m happy to implement Phase 1 (and 2 if you agree) in a follow‑up PR.

