# Menu / Inventory Property Test

This test exercises game-style menu and inventory scenes in Nuklear:

- two overlapping top-level windows (`menu`, `inventory`)
- nested `nk_popup_begin` windows plus nested `nk_group_begin_titled` groups
- clipped groups with borders, hidden scrollbars, and overflowing content
- controls alternating between visible and hidden
- windows alternating between visible, hidden, and minimized
- four palettes switched every frame with balanced style push/pop calls

Each of the 200 deterministic cases renders 12 frames. Every frame is rendered
twice with independent contexts and tracking allocators. The test compares the
complete command stream and layout rectangle list from both contexts.

## Properties

- no crash under normal and ASan/UBSan builds
- every Nuklear allocation is returned by `nk_free`
- no invalid/double frees or allocator realloc misuse
- command stream is deterministic for the same scene/frame
- layout rectangles are deterministic, finite, and non-negative
- all real scissor rectangles stay inside the 1280x720 viewport
- style stacks return to their baseline after every frame
- visible controls whose ancestor containers actually opened have layout slots
- hidden controls do not receive layout slots

Failing scenes are reduced by deleting subtrees. The smallest failing tree is
printed to stdout and to `artifacts/counterexamples.txt`.

## Run

```sh
cd tests
make test
make asan
```

Or run the binary directly from the repository root:

```sh
tests/menu_inventory_props --cases 200 --frames 12 \
  --frames-out artifacts/property_frames.ndjson \
  --summary-out artifacts/property_summary.json \
  --failures-out artifacts/counterexamples.txt
```

A single case can be reproduced with:

```sh
tests/menu_inventory_props --case 73 --frames 12 --no-frames \
  --summary-out /tmp/summary.json --failures-out /tmp/failures.txt
```

## Output

- `artifacts/property_frames.ndjson`: one JSON object per frame with every
  draw command, its rectangle/color/text, every recorded layout rectangle and
  clip rectangle, hidden nodes, and active containers.
- `artifacts/property_summary.json`: aggregate counts and property results.
- `artifacts/counterexamples.txt`: minimized failing trees, or a note that no
  counterexamples were found.

## Current Result

The deterministic run completed 200 cases and 2400 frames:

- 76,717 draw commands recorded
- 25,753 layout rectangles recorded
- 808 allocations / 808 frees, 0 live bytes after `nk_free`
- 0 crashes, 0 leaks, 0 deterministic-layout mismatches
- 0 counterexamples

ASan/UBSan with leak detection produces the same command/layout totals and also
reports no errors. A second process run produces a byte-identical
`property_frames.ndjson`, so the recorded stream is independent of ASLR and
heap addresses.

Nuklear forbids nesting `nk_begin_xxx` calls, so the scene tree uses nested
popup windows and groups. This keeps the nested-window cases within the
toolkit's supported API while still exercising nested panels, clip unification,
and popup command ordering.
