# Engine patches

`engine/quickjs-ng` is a git submodule tracking upstream QuickJS-NG unchanged.
Every change we make to the engine lives here as a patch, applied to that
submodule's working tree by `scripts/apply-patches.js`.

The table below is the whole fork at a glance. You should be able to understand
what we changed and why without opening a single `.patch` file.

## The patches

| Patch | What it does | Why we need it | Adds | Bytecode bump? |
|---|---|---|---|---|
| `0001` | Reports how much memory the engine is currently holding | The runtime decides when to collect garbage based on how much memory is live, and the existing way to measure it walks the whole heap | `JS_GetMallocSize` | No |
| `0002` | Lets the app choose when garbage collection runs | Otherwise a collection lands in the middle of a frame and drops it; the worst frame went from 45ms to 3.3ms | `JS_SetGCDeferred`, `JS_HasPendingGC`, `JS_RunPendingGC` | No |
| `0003` | Lets a debugger pause code, read variables and evaluate expressions | Without it no debugger can attach to an app running on this engine, and `debugger;` does nothing | `JS_SetDebugTraceHandler`, `JS_GetStackDepth`, `JS_GetLocalVariablesAtLevel`, `JS_FreeLocalVariables`, `JS_SetVariableAtLevel`, `JS_EvalInStackFrame` | **Yes** - adds an opcode |
| `0004` | Lets the debugger ask where each stack frame is | Chrome DevTools shows a full call stack, not just the line that stopped | `JS_GetFrameInfoAtLevel` | No |
| `0005` | Turns the debugger's per-statement callback on and off cheaply | A debugger must stay installed all session; idle, the callback cost 4,040ns per statement against 546ns with none | `JS_SetDebugTraceArmed` | No |
| `0006` | Lets us attach native data to a JavaScript object where script cannot see or overwrite it | The engine's embedder slot only exists on objects the embedder created, and this data attaches to any object. The alternative, a WeakMap, enters JavaScript on every read and write: reads went 11.1ns to 4.4ns, writes 59.5ns to 8.6ns | `JS_NewPrivateSymbol` | No |
| `0007` | Makes a stripped bytecode function stringify as `[stripped source]` rather than `[native code]` | Stripping the source text made every affected function indistinguishable from a C built-in, which is a lie that confuses debugging | - | No |
| `0008` | Speeds up JSON.parse and JSON.stringify on mobile-real payloads: integer arrays, plain ASCII strings, small nested objects | JSON is what native bindings and the bridge use, so its hot shapes dominate real payloads; the old code allocated per character and per element | - | No |
| `0009` | Speeds up JSON.stringify: enumerate object keys as atoms, quote keys and strings straight into the buffer, and format floats/booleans/null directly (integral doubles exact within +/-2^53) | JSON.stringify dominates network payloads; the old code built a key array and an intermediate quoted string per key and per value | - | No |
| `0010` | Faster json tokenizer for structural punctuation: `[ ] { } : ,` return directly without per-token state bookkeeping | Punctuation is 50-80% of JSON tokens, and each previously paid the full tokenizer update for a byte the parse loop just compares against | - | No |
| `0011` | Lazy JSON.parse with shared source, structural tape, direct markers, repeated-layout construction, and access feedback | Large payloads are often only partly inspected; eager parsing pays for values that are never read, while the lazy document keeps source and tape ownership safe for retained descendants | - | No |
| `0012` | Caches shapes for eligible static object literals and serializes their reusable metadata with bytecode | Repeated object-literal sites otherwise rebuild the same property shape through one transition per property | - | Yes |
| `0013` | Accelerates forward string `indexOf` and `includes` searches with `memchr` and same-width `memcmp` | Forward searches repeatedly scan for candidate bytes and compare matching-width strings; libc can perform both operations efficiently | - | No |
| `0014` | Reclaims one primary opcode slot with a compact escape prefix for debugger traps | The one-byte opcode table is full, so the escape form provides room for later engine opcodes without changing ordinary dispatch | `esc1` | **Yes** - changes bytecode encoding |
| `0015` | Adds the foundational monomorphic property and transition-store inline caches, with lazy tables and a cheap megamorphic call-site guard | React Native repeatedly reads and initializes the same object fields; caching the receiver shape avoids repeating hashed property lookup while unsafe cases keep the generic path | `get_field_ic`, `get_field2_ic`, `put_field_ic` | **Yes** - adds opcodes |
| `0016` | Composes explicitly enabled four-entry sites, depth-two prototype reads, and a non-owning megamorphic fallback cache on top of the foundational inline caches | Polymorphic and megamorphic workloads can revisit a small set of receiver shapes and prototype paths often enough to justify the optional wider cache | - | No |
| `0017` | Makes the four-entry inline-cache configuration the shipping default | The validated composed cache was measured at width four on device, while the release projection still defaulted to width one | - | No |
| `0018` | Adds bounded metadata for regular-expression optimization | The matcher needs safe, serialized search information for supported literal and prefix patterns | - | **Yes** - extends serialized regular-expression bytecode |
| `0019` | Adds specialized regular-expression matcher execution | Common eight-bit expressions should not pay for wide-character handling on every operation | - | No |
| `0020` | Analyzes regular expressions for literal, prefix, and first-set prefilters | Search-heavy expressions reject most input positions before a match can begin | - | **Yes** - emits regular-expression metadata |
| `0021` | Connects regular-expression metadata to JavaScript matching | Compiled metadata only helps when RegExp operations use it to skip impossible candidates | - | No |
| `0022` | Reduces setup and capture overhead in regular-expression execution | Repeated execution otherwise rebuilds state and allocates capture storage for common small expressions | - | No |
| `0023` | Optimizes regular-expression result arrays and boolean tests | Successful matches with captures pay unnecessary property-growth and result-object costs | - | No |
| `0024` | Adds a fast scanning path for `RegExp[Symbol.split]` and recognizes whitespace first sets | Splitting otherwise invokes the sticky matcher once per source position, while whitespace-led patterns lose the prefilter entirely | - | No |
| `0025` | Interns the six non-predefined RegExp flag property names once per runtime | Repeated `.flags` reads otherwise redo atom-table lookups for names that are already permanent atoms | - | No |
| `0026` | Derives RegExp flags directly from compiled bytecode under a semantic guard | Standard RegExp instances can answer `.flags` without eight generic prototype lookups and native getter calls | - | No |
| `0027` | Keeps the malformed RegExp test fixture synchronized with the opcode table and tightens its assertion | Adding matcher opcodes must not silently turn a bounds-check regression test into a different bytecode program | - | No |
| `0028` | Hardens RegExp metadata validation and keeps first-set scans interruptible | Deserialized metadata must not drive out-of-bounds fast-path reads, and large no-hit scans must remain interruptible | - | No |
| `0029` | Adds regression coverage for malformed RegExp metadata | Header mutations must be rejected before optimized execution can consume them | - | No |
| `0030` | Replaces the serialized-bytecode checksum with portable CRC-32C and uses hardware acceleration when available | Bytecode loading scans the full payload before parsing, so faster integrity verification reduces cold-start cost while preserving corruption detection | - | **Yes** - changes checksum encoding |
| `0031` | Frames serialized function bodies and provides one checked bounded reader for eager and lazy-ready records | A declared body boundary and shared overflow-safe layout prevent malformed function records from consuming adjacent data while allowing later lazy materialization to use the same parser | - | **Yes** - changes bytecode record layout |
| `0032` | Adds explicit caller-owned borrowed storage for lazy bytecode input | Embedders with stable immutable storage can avoid the engine-owned payload copy without changing the safe copied-lazy default | `JS_READ_OBJ_BORROW` | No |
| `9999` | Raises the bytecode version number, once, for every patch above that needs it | Bytecode built by a patched engine must not load in an unpatched one. Doing it here rather than in each patch stops patches colliding on the same line | - | This is the bump |

## Reading the patches

`patch_viewer.html` renders every `.patch` in this directory side by side, with
the header, the diff and syntax highlighting. Open it from a local server so it
can list the directory, or drag the patch files onto it:

```
python3 -m http.server -d engine/patches
```

## Writing a patch header

Headers are for people. Keep them short -- usually twenty to forty lines --
and say things the way you would say them out loud.

```
Subject: <one sentence saying what this adds>

What this does
  Two to four short sentences.

Why we need it
  What in react-native-quickjs is broken or impossible without this.

What it adds
  JS_GetFrameInfoAtLevel()  - asks the engine where a given stack frame is
                              in the source: which file, which line, which
                              column, and the function's name.

Needs a bytecode version bump
  No.

Notes
  Anything a reviewer genuinely needs. Leave it out if there is nothing.
```

Rules:

- Spell acronyms out the first time. Write "the Chrome DevTools debugging
  protocol", not "CDP".
- No decoration. No symbols, no shouting in capitals, no bookkeeping about
  which branch or session produced it.
- Nothing a stranger cannot follow. No references to internal documents, and
  no using a patch number as if it were a name.
- Describe behaviour, not internals. "Lets the debugger ask where each stack
  frame is" beats a paragraph about borrowed atoms. Details like who frees
  what go under **Notes**, if they matter at all.
- If the patch exists for speed, show the measurement.

## No patch bumps the bytecode version

A patch that changes the bytecode format **must not touch `BC_VERSION`**. It
says so in its header and in its README row, and changes nothing.

One patch raises the number, once, for the whole series:

    9999-bc-version-bump.patch

It is numbered `9999` so that adding patches never renumbers it and it always
applies last. Its header lists every patch that made the bump necessary.

The reason is not tidiness. If each patch bumps the version itself, then any
two such patches conflict over the same line for reasons that have nothing to
do with what either one does, and rebasing the series becomes a day's work
instead of a minute's.

The precompiled `builtin-*.h` blobs record the bytecode version in their first
byte, so they are regenerated *after* the tail patch, as part of the projection
step — never inside the patch that changed an opcode.

`scripts/apply-patches.js --check` enforces all of this. It fails if a patch
has no row in this table, if a row names a patch that does not exist, if any
patch other than `9999` touches `BC_VERSION`, or if a patch says it needs a
bump while `9999` is missing.
