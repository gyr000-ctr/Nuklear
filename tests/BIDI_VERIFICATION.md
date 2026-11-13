# Bidirectional Text Support — Verification Record

This note records how the UAX #9 bidirectional text implementation was
verified, the commands used and the observed results.

## What was added

- `src/nuklear_bidi.c` — bidi engine (compiled only with `NK_INCLUDE_BIDI`)
  - P2/P3 paragraph direction, X1–X8 embedding/override/isolate controls
  - W1–W7 weak rules (European/Arabic numbers, separators, NSM)
  - simplified N0 bracket pairs, N1–N3 neutral rules
  - I1/I2 implicit levels, L1 whitespace levels, L2 visual reordering
  - L4 mirrored punctuation
  - persistent caches: storage only grows, steady-state layout allocates
    no heap memory proportional to text length
  - every failure path (bad UTF-8, overlong input, allocator failure,
    unmatched controls) degrades to plain LTR instead of crashing
- Public API in `src/nuklear.h`: `nk_bidi_enable`, `nk_bidi_visual`,
  `nk_bidi_wrap_lines`, `nk_bidi_hit_test`, `nk_bidi_caret_x`,
  `nk_bidi_visual_neighbor`, `nk_bidi_selection_rects`,
  `nk_bidi_visual_segments`, `nk_bidi_line_count`, `nk_bidi_line_info`,
  `nk_bidi_text_direction`, `nk_bidi_line_direction`.
- Integration in `nuklear_text.c` (labels, wrapping, clipping),
  `nuklear_edit.c` and `nuklear_text_editor.c` (caret, selection, hit
  testing, scrolling). All changes are inside `#ifdef NK_INCLUDE_BIDI`;
  with the macro undefined the preprocessed output contains zero bidi
  symbols, so existing LTR behavior, the font atlas and widget APIs are
  unchanged. Runtime bidi is also off until `nk_bidi_enable(nk_true)`.

## Test commands and results

Run from the repository root unless noted.

### 1. Regenerate the amalgamated header

    make nuke

The regenerated `nuklear.h` is stable across repeated runs (idempotent).

### 2. Unit + integration tests

    cd tests && make test

Result:

    bidi_test:           3133 checks, 0 failures
    bidi_integration_test: 8 checks, 0 failures

Coverage includes Arabic/Hebrew/Latin/digit mixing, mirrored
punctuation, nested and unmatched embedding controls, empty paragraphs,
malformed UTF-8, overlong runs, word wrapping/clipping, caret hit
testing, logical<->visual neighbor mapping, selection extents, text
editor click/key behavior, cache reuse, and byte-identical LTR output
while bidi is disabled.

### 3. Address/undefined behavior sanitizers

    gcc -std=c99 -g -fsanitize=address,undefined \
        -DNK_INCLUDE_FIXED_TYPES -DNK_INCLUDE_STANDARD_IO \
        -DNK_INCLUDE_STANDARD_VARARGS -DNK_INCLUDE_DEFAULT_ALLOCATOR \
        -DNK_INCLUDE_VERTEX_BUFFER_OUTPUT -DNK_INCLUDE_FONT_BAKING \
        -DNK_INCLUDE_DEFAULT_FONT -DNK_INCLUDE_BIDI -DNK_IMPLEMENTATION \
        -I. tests/bidi_test.c -o /tmp/t1 -lm && /tmp/t1
    gcc ...same defines... tests/bidi_integration_test.c -o /tmp/t2 -lm \
        && /tmp/t2

Result: both binaries report 0 failures with no ASan/UBSan/leak reports.

### 4. No per-frame heap allocation in steady state

A driver (`mallinfo2`) runs 10000 iterations of `nk_bidi_visual`,
`nk_bidi_wrap_lines`, `nk_bidi_caret_x` and `nk_bidi_hit_test` on the
same text after a warm-up:

    heap before=2512 after=2512 delta=0 ZERO-ALLOC STEADY STATE

### 5. Strict C89 compilation (the project's CI standard)

    gcc -std=c89 -Wall -Wextra -pedantic -Werror \
        -DNK_INCLUDE_FIXED_TYPES -DNK_INCLUDE_DEFAULT_ALLOCATOR \
        -DNK_INCLUDE_STANDARD_IO -DNK_INCLUDE_STANDARD_VARARGS \
        -DNK_INCLUDE_VERTEX_BUFFER_OUTPUT -DNK_INCLUDE_BIDI \
        -I src -c src/nuklear_bidi.c   # likewise for the 3 edited files

Result: clean, no warnings.

### 6. Project CI source compile (all 44 translation units)

    sh -e ./.github/ci_compile_sources.sh

Result: exit 0, every `src/*.c` compiles.

### 7. No-bidi build of the amalgamated header

    gcc -std=c89 -Wall -Wextra -pedantic \
        -DNK_INCLUDE_FIXED_TYPES -DNK_INCLUDE_DEFAULT_ALLOCATOR \
        -DNK_INCLUDE_STANDARD_IO -DNK_INCLUDE_VERTEX_BUFFER_OUTPUT \
        -DNK_IMPLEMENTATION -I. -c nuklear.h

Result: compiles cleanly; preprocessed text/edit/editor sources contain
no `nk_bidi` symbols when `NK_INCLUDE_BIDI` is undefined.

### 8. Real application links in both configurations

    make -C demo/rawfb/x11                       # default (bidi off)
    cc demo/rawfb/x11/main.c ... -DNK_INCLUDE_BIDI -lX11 -lXext -lm

Result: both binaries build and link successfully.

## Changed files

- new: `src/nuklear_bidi.c`, `tests/bidi_test.c`,
  `tests/bidi_integration_test.c`, `tests/Makefile`
- edited: `src/nuklear.h` (public API), `src/nuklear_text.c`,
  `src/nuklear_edit.c`, `src/nuklear_text_editor.c`, `Makefile`
  (pack list), regenerated `nuklear.h`
