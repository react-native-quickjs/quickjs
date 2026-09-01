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
| `9999` | Raises the bytecode version number, once, for every patch above that needs it | Bytecode built by a patched engine must not load in an unpatched one. Doing it here rather than in each patch stops patches colliding on the same line | - | This is the bump |

## Writing a patch header

Headers are for people. Keep them short and say things the way you would say
them out loud.

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
