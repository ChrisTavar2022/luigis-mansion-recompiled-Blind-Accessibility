# Decompilation helpers

These work against a local lm-decomp checkout whose `build/GLME01/asm` has been
generated from your own disc (see docs/PORTING.md). Point `LM_WORK` at the directory
containing `lm-decomp/` (and `m2c/` for m2c_run.py).

- `m2c_run.py fn...` — m2c pseudo-C for functions (fixes dtk jump tables for m2c)
- `describe.py fn...` — size, call-graph size and strings referenced near a function
- `callgraph.py fn [depth] [x]` — transitive callees (excluding SDK/JSystem)
