# Engine patches

`engine/quickjs-ng` is a git submodule tracking upstream QuickJS-NG unchanged.
Every change we make to the engine lives here as a patch, applied to that
submodule's working tree by `scripts/apply-patches.js`.

The table below is the whole fork at a glance. You should be able to understand
what we changed and why without opening a single `.patch` file.

## The patches

| Patch | What it does | Why we need it | Adds | Bytecode bump? |
|---|---|---|---|---|

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
