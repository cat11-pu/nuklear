# Nuklear command-pipeline benchmark

The benchmark builds the same synthetic UI against two versions of
`nuklear.h`: the current `HEAD` revision and the working tree. It does not
open a window or use a GPU.

Each frame clears the context, lays out the configured number of windows,
converts the command list to vertices and indices, and records:

- `avg_ms`: total layout + command generation + `nk_convert` time.
- `gen_ms`: context clear, input, layout, and command generation.
- `convert_ms`: command-to-vertex conversion.
- `commands`: abstract Nuklear draw commands.
- `convert_allocs`: vertex/index/command-buffer allocation calls per frame.
- `draw_commands`: backend draw batches after `nk_convert`.
- `vertices` / `elements`: vertex and index counts.
- `hash`: FNV-1a hash of a deterministic 1280x800 RGBA software raster.

Every third window is minimized. The optimized path only emits its header,
while visible windows contain labels, buttons, progress bars, sliders,
checkboxes, and scrollbars.

## Run

```sh
make -C bench compare
make -C bench verify
```

`compare` runs 32, 64, and 128 windows and uses `cmp` on the raw RGBA output.
`verify` additionally checks a 130-point path that exceeds the inline path
buffer, a relaxed float-color vertex layout, and a generic fallback layout.
All verification outputs must compare byte-for-byte.

## Reference result

Measured on one machine with GCC 15.2, `-O3 -DNDEBUG`, 500 frames per run,
five runs per tier, one third of the windows minimized, and the median
reported:

| Windows | HEAD (ms/frame) | Optimized (ms/frame) | Reduction | Pixels |
|---:|---:|---:|---:|---|
| 32 | 0.388 | 0.148 | 61.9% | byte-identical |
| 64 | 0.773 | 0.297 | 61.6% | byte-identical |
| 128 | 1.562 | 0.581 | 62.8% | byte-identical |

The raw RGBA hashes for the three tiers are `c0136d81141ff37d`,
`8e06f53fc3de24f7`, and `e72b74448f5be4e9`.
