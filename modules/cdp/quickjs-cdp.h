/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The Chrome DevTools Protocol, for QuickJS.
 *
 * This is plain C over <quickjs.h> and nothing else, with no generated
 * marshalling code: protocol messages are JSON, and
 * the engine already has a JSON parser, so they are handled as JSValues on a
 * private context of the agent's own.
 *
 * THREADS
 *   Every protocol message is handled on the JS thread. qjs_cdp_send_message()
 *   may be called from any thread; it copies the message and queues it. The
 *   queue is drained by qjs_cdp_poll(), which the embedder calls on the JS
 *   thread, and by the pause loop, which is the only reason the queue exists:
 *   while execution is stopped at a breakpoint the JS thread is inside the
 *   engine, and the frontend still has to be answered.
 *
 * PARSE-TIME INSTRUMENTATION
 *   The engine emits statement traps only into code parsed while a trace
 *   handler is installed, so qjs_cdp_new() must be called before any script is
 *   evaluated. A runtime that started without one cannot be attached to later.
 */

#ifndef QUICKJS_CDP_H
#define QUICKJS_CDP_H

#include <quickjs.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QJSCDPAgent QJSCDPAgent;

/* Receives one complete CDP message to deliver to the frontend. Called on the
   JS thread. `json` is owned by the agent and is not valid after the call.

   `session` is the token that came with the request being answered, or NULL for
   an event, which belongs to every session at once. A reply must reach only the
   frontend that asked: two frontends can be attached, and answering both makes
   each of them see a response to something it never sent. */
typedef void QJSCDPSendFunc(
    void *opaque, void *session, const char *json, size_t len);

/* Creates the agent and installs the engine's trace handler on `ctx`.
   `execution_context_id` is the id reported in Runtime.executionContextCreated
   and echoed back in every remote object. */
QJSCDPAgent *qjs_cdp_new(
    JSContext *ctx, int execution_context_id, QJSCDPSendFunc *send,
    void *send_opaque);

void qjs_cdp_free(QJSCDPAgent *agent);

/* True when this agent implements `method`.
   A request it does not implement must be left to whoever else is listening
   rather than answered with an error: the frontend talks to several agents at
   once, and refusing on behalf of all of them is not ours to do. */
bool qjs_cdp_handles(const char *method);

/* Tells the agent which execution context it is speaking for. The embedder
   assigns the id, and a Runtime.evaluate naming any other context is refused
   rather than run in this one. */
void qjs_cdp_set_execution_context(QJSCDPAgent *agent, int id);

/* Queues one CDP message from `session`. Any thread. */
void qjs_cdp_send_message(
    QJSCDPAgent *agent, void *session, const char *json, size_t len);

/* Handles every queued message. JS thread only. */
void qjs_cdp_poll(QJSCDPAgent *agent);

/* Reports a console call to the frontend as Runtime.consoleAPICalled. `type` is
   the protocol's name for it ("log", "warning", ...). The values are borrowed
   for the duration of the call. JS thread only. */
void qjs_cdp_console_message(
    QJSCDPAgent *agent, const char *type, const JSValue *args, int count);

/* Tells the agent a script was loaded, so it can answer Debugger.scriptParsed
   and resolve breakpoints set against `url`. Call after evaluating each script.
   JS thread only. */
void qjs_cdp_script_loaded(
    QJSCDPAgent *agent, const char *url, const char *source);

/* Everything a replacement agent needs to carry on where this one left off:
   whether the debugger was enabled and which breakpoints the frontend has set.
   React Native destroys and recreates the runtime on a reload, and the frontend
   is not told -- from its side the breakpoints it set are still set.

   Export returns a string the caller frees with free(); import takes one back. */
char *qjs_cdp_export_state(QJSCDPAgent *agent);
void qjs_cdp_import_state(QJSCDPAgent *agent, const char *json);

/* Whether the debugger is switched on for the runtime.

   Debugger.enable turns it on by itself, so a lone embedder needs none of this.
   Turning it OFF is different: enabling is per session and two frontends can
   disagree, so Debugger.disable from one of them is not the whole story. An
   embedder that supports several sessions calls the setter with the union.
   After an import this is whatever it was before the reload. */
bool qjs_cdp_debugger_enabled(QJSCDPAgent *agent);
void qjs_cdp_set_debugger_enabled(QJSCDPAgent *agent, bool enabled);

/* The call stack right now, as a Runtime.StackTrace JSON object. Returns a
   string the caller frees with free(), or NULL when nothing is running.
   `frames_to_skip` drops that many innermost frames, which is how a native
   function leaves itself out of the trace it is capturing. JS thread only. */
char *qjs_cdp_capture_stack_trace(QJSCDPAgent *agent, int frames_to_skip);

/* Asks the running program to stop at the next statement. Any thread. */
void qjs_cdp_pause(QJSCDPAgent *agent);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_CDP_H */
