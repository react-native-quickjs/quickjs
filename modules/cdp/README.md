# The Chrome DevTools Protocol, for QuickJS

Two files. `quickjs-cdp.h` is the API, `quickjs-cdp.c` is the whole
implementation — about 1,360 lines of C over `<quickjs.h>` and nothing else.

## What the protocol is

Chrome DevTools debugs a JavaScript engine over a WebSocket, in JSON. Requests
carry an `id` and get a reply with the same `id`; the engine also volunteers
events, which have no `id`:

```
->  {"id":7,"method":"Debugger.setBreakpointByUrl","params":{"url":"app.js","lineNumber":12}}
<-  {"id":7,"result":{"breakpointId":"1","locations":[...]}}
<-  {"method":"Debugger.paused","params":{"callFrames":[...],"reason":"other"}}
```

That is the whole of it. Methods are grouped into domains — `Runtime` for
evaluating expressions and inspecting values, `Debugger` for breakpoints,
stepping and stack frames — and the domain is part of the method name.
Everything below is in service of those three lines.

## The idea that keeps it small

**The engine already parses JSON.** So there is no JSON library here, and no
generated marshalling code either. A message is parsed with `JS_ParseJSON` into
a `JSValue`, a reply is a `JSValue` built by setting properties, and
`JS_JSONStringify` turns it back into text.

Protocol JSON is parsed on **a second, private QuickJS runtime** that never
runs a program. It exists so that answering a message cannot allocate in,
collect from, or throw into the heap the debugger is supposed to be observing.
It is built with base objects and JSON only, so it is small.

## How pausing works

This is the only genuinely delicate part.

The engine emits a trap at every statement boundary and calls `on_statement`.
Almost always the answer is "keep going" and the function returns immediately.
When it decides to stop — a breakpoint matched, a step finished, the frontend
asked — it does **not** return. It calls `pause_until_resumed`, which sends
`Debugger.paused` and then loops, handling protocol messages, until something
tells it to continue.

So while execution is stopped the JS thread is *inside the engine*, half way
down `JS_Eval`. It cannot go back to whatever normally pumps the message queue,
which is exactly why the queue exists:

```
any thread                  JS thread
----------                  ---------
qjs_cdp_send_message  --->  queue  ---> pause_until_resumed drains it
                                        (or qjs_cdp_poll, when not paused)
```

`Debugger.resume` and the three step methods set a flag and signal a condition
variable; the loop notices, returns, and the engine carries on from the
statement it stopped at. Stepping is then a comparison against the stack depth
recorded when the loop exited:

| mode | stops at the next statement where |
|---|---|
| `MODE_STEP_INTO` | anywhere |
| `MODE_STEP_OVER` | depth is no deeper than where we resumed |
| `MODE_STEP_OUT` | depth is shallower than where we resumed |

## Instrumentation is decided before anything runs

The engine emits those statement traps at **parse time**, and only into code
compiled while a trace handler was installed. `qjs_cdp_new` installs one, so it
has to be called before any script is evaluated. A runtime that started without
one cannot be attached to later — there are no traps in its bytecode to attach
to.

`Debugger.disable` therefore *disarms* the handler rather than removing it.
Removing it would permanently un-instrument everything parsed afterwards.

## What is in the file

| | |
|---|---|
| JSON helpers | `json_object`, `json_set_string`, `json_get_int`, … over the private runtime |
| Sending | `send_result`, `send_error`, `send_event` |
| Remote objects | the `objectId` ↔ live `JSValue` table the frontend holds references through |
| `to_remote_object` | a `JSValue` as the frontend wants to see it: type, class, description |
| Scripts and breakpoints | plain arrays; a breakpoint URL matches any script URL ending with it |
| `call_frames` | the stack, from `JS_GetFrameInfoAtLevel` and `JS_GetLocalVariablesAtLevel` |
| Pausing | `on_statement`, `pause_until_resumed`, `resume_with` |
| `dispatch` | one `else if` per method. No tables, no registration |

The protocol counts lines and columns from zero and the engine counts from one,
so every conversion is explicit at the point it happens. Getting it wrong shows
up as "breakpoints are off by one", never as a crash.

## Editing a variable in a paused frame

`Debugger.setVariableValue` writes a local in a frame that is stopped, and the
program then uses the new value. The engine patch that added
`JS_GetLocalVariablesAtLevel` added `JS_SetVariableAtLevel` alongside it, so the
frontend's "edit value" reaches the live frame rather than a copy of it:

```
paused in add(2, 3), set y = 40, resume  ->  result is 42
```

## Testing

`test/cdp_test.cpp` is the fast gate: it asserts on the JSON text, because that
text is the contract and a test that reached inside the agent could pass while
the wire format was wrong.

The real measure is somebody else's suite. `test/rn-inspector/` builds React
Native's own `JsiIntegrationTest.cpp` out of `node_modules`, unmodified, and
points its engine list at this one; `generate-suite.mjs` redirects the three
lines that name an engine adapter and changes nothing else. A failing-test list
from a suite we did not write is a work plan in a way our own assertions are
not.

Measured 2026-09-01, macOS arm64: **26 pass, 1 fail, 0 crash of 27.**

The one failure is `testCaptureAndSerializeStackTrace`, and only its column
numbers: we report the first character of the callee where the suite expects the
opening paren. Frame count, function names, script ids and line numbers all
match. The engine records a frame's column from the pc2line table at the call
instruction, so closing this means changing what the engine records, not what
this layer reports.

## What it does not do yet

Worth knowing, because several of these are accepted and quietly ignored rather
than refused -- the frontend gets an empty success and nothing happens:

| | |
|---|---|
| `Debugger.setPauseOnExceptions` | accepted, ignored. The engine has no throw hook to hang it on |
| `Debugger.setBlackboxPatterns`, `setBlackboxedRanges` | accepted, ignored |
| `Debugger.setSkipAllPauses` | accepted, ignored |
| breakpoint `condition` | stored and handed back, never evaluated. A conditional breakpoint stops every time |
| `Debugger.setBreakpoint` | not implemented. Breakpoints are set by URL only |
| `Runtime.callFunctionOn`, `compileScript`, `globalLexicalScopeNames` | not implemented |

## Roadmap

Roughly in order of value for effort. The first group needs nothing from the
engine that is not already there.

**Cheap, and the engine already supports them**

- **Evaluate breakpoint conditions.** The condition is already stored; this is
  a `JS_EvalInStackFrame` at the trap and a check of the result.
- **`Debugger.setSkipAllPauses`** -- one flag consulted in `on_statement`.
- **`Debugger.continueToLocation`** -- a one-shot breakpoint, then resume.
- **`Debugger.setBreakpoint`** -- the same as `setBreakpointByUrl` addressed by
  scriptId instead of URL.
- **`Runtime.terminateExecution`** -- `JS_SetInterruptHandler` already exists to
  stop a running program.
- **`Runtime.callFunctionOn`** -- call a function against an object the frontend
  holds by objectId. Used by DevTools when expanding values.

**Needs a little engine work**

- **`Debugger.getPossibleBreakpoints`** -- answerable properly from the pc2line
  table; today a frontend gets back only the location it asked about.
- **Pause on exceptions.** Needs a seam at `OP_throw` rather than at `JS_Throw`,
  which is called internally and would fire spuriously.
- **Stack trace columns**, to match what other engines report.

**Whole domains**

- **`Profiler`.** Sample the stack on a timer and return a `Profile` of nodes,
  samples and time deltas. `JS_SetInterruptHandler` already runs periodically on
  the JS thread at a safe point, which is the hard part of sampling solved.
- **`HeapProfiler`.** `takeHeapSnapshot` streams a column-oriented document --
  flat integer arrays of nodes and edges over a string table -- built by a
  traversal shaped like the mark phase. Note that this needs backpressure, not
  just a chunk loop: React Native has an open bug where snapshots over about
  100 MB overflow the WebSocket send queue and close the connection.

**Probably not**

`Debugger.setScriptSource` (live edit), `restartFrame` and `setReturnValue` all
need the engine to rewrite or unwind a live frame, which QuickJS cannot do.

## Using it


```c
QJSCDPAgent *agent = qjs_cdp_new(ctx, 1, send_to_frontend, socket);

qjs_cdp_send_message(agent, text, len);   /* any thread */
qjs_cdp_poll(agent);                      /* JS thread */

qjs_cdp_script_loaded(agent, url, source);  /* after evaluating each script */
```
