# Coding Style and Documentation Guidelines

This project follows a clear, consistent style to make real‑time DSP code easy to read and safe to modify.

## Formatting

- Braces: Allman style (opening brace on a new line) — enforced by `.clang-format`.
- Indentation: 4 spaces. Tabs are not used.
- Column limit: 100 characters (soft).
- Pointer alignment: `Type *ptr` (left‑aligned `*`).
- No one‑line `if/for/while` bodies — always use braces.

Use your IDE’s “Format Document” or run a clang‑format pass. The configuration lives at the repository root: `.clang-format`.

## File Organization

- One class per file where practical. Keep headers minimal and self‑contained.
- Include order: corresponding header, then project headers, then system/Arduino/ESP‑IDF headers.
- Prefer forward declarations in headers to reduce compile time.

## Comments and Documentation

We value context and intent:

- File header: a short paragraph describing the purpose, the pipeline stage, and timing domain (48 kHz, 192 kHz, etc.).
- Class header: responsibility, threading model (which task owns it), and key invariants.
- Public methods: Doxygen‑style comments with purpose, parameters, and effects.
- Internal functions: explain non‑obvious math, optimizations (SIMD, alignment), and edge cases.
- Real‑time notes: explicitly call out critical sections, memory allocation assumptions (no heap in RT paths), and per‑block complexity.

Example Doxygen header:

```cpp
/**
 * MPXMixer
 *
 * Builds the FM stereo MPX: MPX = (L+R) + PILOT*19k + DIFF*38k (DSB‑SC).
 *
 * - Runs at 192 kHz (post‑upsample domain)
 * - No allocations; works in‑place on provided buffers
 * - Vectorizes where profitable, otherwise uses fused scalar loops to minimize memory traffic
 */
```

## FreeRTOS and Tasks

- Start tasks via symmetric module APIs (e.g., `Console::startTask(...)`, `DSP_pipeline::startTask(...)`).
- Avoid blocking operations in real‑time paths. Use queues/message buffers to decouple slow I/O (Serial).
- Prefer fixed‑size messages and no heap allocation in ISR/RT tasks.

## DSP Practices

- State clearly whether a stage runs at 48 kHz (input) or 192 kHz (post‑upsample).
- Document coefficient normalization and expected pass/stopband levels.
- Note SIMD usage (esp‑dsp) and any alignment/`restrict` requirements.
- Clamp only once near the DAC conversion; keep earlier stages unclamped for linearity unless necessary.
