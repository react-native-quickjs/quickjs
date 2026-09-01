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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QJSCDPAgent QJSCDPAgent;

/* Receives one complete CDP message to deliver to the frontend. Called on the
   JS thread. `json` is owned by the agent and is not valid after the call. */
typedef void QJSCDPSendFunc(void *opaque, const char *json, size_t len);

/* Creates the agent and installs the engine's trace handler on `ctx`.
   `execution_context_id` is the id reported in Runtime.executionContextCreated
   and echoed back in every remote object. */
QJSCDPAgent *qjs_cdp_new(
    JSContext *ctx, int execution_context_id, QJSCDPSendFunc *send,
    void *send_opaque);

void qjs_cdp_free(QJSCDPAgent *agent);

/* Queues one CDP message. Any thread. */
void qjs_cdp_send_message(QJSCDPAgent *agent, const char *json, size_t len);

/* Handles every queued message. JS thread only. */
void qjs_cdp_poll(QJSCDPAgent *agent);

/* Tells the agent a script was loaded, so it can answer Debugger.scriptParsed
   and resolve breakpoints set against `url`. Call after evaluating each script.
   JS thread only. */
void qjs_cdp_script_loaded(
    QJSCDPAgent *agent, const char *url, const char *source);

/* Asks the running program to stop at the next statement. Any thread. */
void qjs_cdp_pause(QJSCDPAgent *agent);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_CDP_H */
