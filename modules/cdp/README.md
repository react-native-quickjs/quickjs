# The Chrome DevTools Protocol, for QuickJS

Two files. `quickjs-cdp.h` is the API, `quickjs-cdp.c` is the whole
implementation — about 1,100 lines of C over `<quickjs.h>` and nothing else.

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

## Using it

```c
QJSCDPAgent *agent = qjs_cdp_new(ctx, 1, send_to_frontend, socket);

qjs_cdp_send_message(agent, text, len);   /* any thread */
qjs_cdp_poll(agent);                      /* JS thread */

qjs_cdp_script_loaded(agent, url, source);  /* after evaluating each script */
```
