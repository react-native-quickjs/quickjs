/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The Chrome DevTools Protocol, for QuickJS. See quickjs-cdp.h.
 *
 * Protocol messages are JSON and the engine already parses JSON, so there is
 * no JSON library here and no generated marshalling code: a message is a
 * JSValue on a private context, and a reply is built by setting properties on
 * another one.
 */

#include "quickjs-cdp.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int id;
  char *url;
  char *source;
} Script;

typedef struct {
  int id;
  char *url;
  char *condition;
  int line;
  int col;
  bool resolved;
} Breakpoint;

typedef struct {
  int id;
  JSValue value;
  char *group;
} RemoteObject;

typedef enum {
  MODE_RUN,
  MODE_PAUSE_NEXT,
  MODE_STEP_OVER,
  MODE_STEP_INTO,
  MODE_STEP_OUT
} PauseMode;

struct QJSCDPAgent {
  JSContext *ctx;

  /* Protocol JSON is parsed and printed on a runtime of the agent's own, so
     that handling a message cannot allocate in, collect from or throw into the
     heap the debugger exists to observe. */
  JSRuntime *jrt;
  JSContext *jctx;

  int exec_ctx_id;
  QJSCDPSendFunc *send;
  void *send_opaque;

  /* Messages arrive on the inspector thread and are handled on the JS thread.
     The condition variable is what lets the pause loop sleep rather than spin. */
  pthread_mutex_t lock;
  pthread_cond_t cond;
  char **queue;
  int queue_len, queue_cap;

  Script *scripts;
  int nscripts;
  Breakpoint *bps;
  int nbps, next_bp_id;
  RemoteObject *objs;
  int nobjs, next_obj_id;

  bool debugger_enabled;
  bool runtime_enabled;
  bool breakpoints_active;

  PauseMode mode;
  int step_depth;
  bool paused;
  bool resumed;
  bool pause_pending;
};

/* ---------------------------------------------------------------- JSON --- */

static JSValue json_object(QJSCDPAgent *agent) {
  return JS_NewObject(agent->jctx);
}

static JSValue json_array(QJSCDPAgent *agent) {
  return JS_NewArray(agent->jctx);
}

static void json_set(QJSCDPAgent *agent, JSValue o, const char *k, JSValue v) {
  JS_SetPropertyStr(agent->jctx, o, k, v);
}

static void json_set_string(
    QJSCDPAgent *agent, JSValue o, const char *k, const char *v) {
  json_set(agent, o, k, JS_NewString(agent->jctx, v ? v : ""));
}

static void json_set_int(
    QJSCDPAgent *agent, JSValue o, const char *k, int64_t v) {
  json_set(agent, o, k, JS_NewInt64(agent->jctx, v));
}

static void json_set_bool(
    QJSCDPAgent *agent, JSValue o, const char *k, bool v) {
  json_set(agent, o, k, JS_NewBool(agent->jctx, v));
}

static void json_set_index(
    QJSCDPAgent *agent, JSValue arr, uint32_t i, JSValue v) {
  JS_SetPropertyUint32(agent->jctx, arr, i, v);
}

static JSValue json_get(QJSCDPAgent *agent, JSValue o, const char *k) {
  return JS_GetPropertyStr(agent->jctx, o, k);
}

/* Result must be released with JS_FreeCString(agent->jctx, ...). NULL when the
   property is absent or null. */
static const char *json_get_string(
    QJSCDPAgent *agent, JSValue o, const char *k) {
  JSValue v = json_get(agent, o, k);
  if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v)) {
    JS_FreeValue(agent->jctx, v);
    return NULL;
  }
  const char *s = JS_ToCString(agent->jctx, v);
  JS_FreeValue(agent->jctx, v);
  return s;
}

static int json_get_int(
    QJSCDPAgent *agent, JSValue o, const char *k, int fallback) {
  JSValue v = json_get(agent, o, k);
  int32_t n = fallback;
  if (JS_IsNumber(v)) JS_ToInt32(agent->jctx, &n, v);
  JS_FreeValue(agent->jctx, v);
  return n;
}

static bool json_get_bool(
    QJSCDPAgent *agent, JSValue o, const char *k, bool fallback) {
  JSValue v = json_get(agent, o, k);
  bool b = JS_IsBool(v) ? JS_ToBool(agent->jctx, v) : fallback;
  JS_FreeValue(agent->jctx, v);
  return b;
}

/* Takes ownership of msg. */
static void send_message(QJSCDPAgent *agent, JSValue msg) {
  JSValue s = JS_JSONStringify(agent->jctx, msg, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *text = JS_ToCStringLen(agent->jctx, &len, s);
  if (text) {
    agent->send(agent->send_opaque, text, len);
    JS_FreeCString(agent->jctx, text);
  }
  JS_FreeValue(agent->jctx, s);
  JS_FreeValue(agent->jctx, msg);
}

/* Takes ownership of result. */
static void send_result(QJSCDPAgent *agent, int id, JSValue result) {
  JSValue m = json_object(agent);
  json_set_int(agent, m, "id", id);
  json_set(agent, m, "result", result);
  send_message(agent, m);
}

static void send_empty_result(QJSCDPAgent *agent, int id) {
  send_result(agent, id, json_object(agent));
}

static void send_error(QJSCDPAgent *agent, int id, const char *message) {
  JSValue e = json_object(agent);
  json_set_int(agent, e, "code", -32000);
  json_set_string(agent, e, "message", message);
  JSValue m = json_object(agent);
  json_set_int(agent, m, "id", id);
  json_set(agent, m, "error", e);
  send_message(agent, m);
}

/* Takes ownership of params. */
static void send_event(QJSCDPAgent *agent, const char *method, JSValue params) {
  JSValue m = json_object(agent);
  json_set_string(agent, m, "method", method);
  json_set(agent, m, "params", params);
  send_message(agent, m);
}

/* ------------------------------------------------------- remote objects --- */

static int remember_object(QJSCDPAgent *agent, JSValue v, const char *group) {
  agent->objs = realloc(agent->objs, sizeof(RemoteObject) * (agent->nobjs + 1));
  RemoteObject *r = &agent->objs[agent->nobjs++];
  r->id = ++agent->next_obj_id;
  r->value = JS_DupValue(agent->ctx, v);
  r->group = group ? strdup(group) : NULL;
  return r->id;
}

static RemoteObject *find_object(QJSCDPAgent *agent, const char *object_id) {
  if (!object_id) return NULL;
  int id = atoi(object_id);
  for (int i = 0; i < agent->nobjs; i++)
    if (agent->objs[i].id == id) return &agent->objs[i];
  return NULL;
}

static void forget_object(QJSCDPAgent *agent, int i) {
  JS_FreeValue(agent->ctx, agent->objs[i].value);
  free(agent->objs[i].group);
  agent->objs[i] = agent->objs[--agent->nobjs];
}

static void release_object(QJSCDPAgent *agent, const char *object_id) {
  RemoteObject *r = find_object(agent, object_id);
  if (r) forget_object(agent, (int)(r - agent->objs));
}

static void release_object_group(QJSCDPAgent *agent, const char *group) {
  if (!group) return;
  for (int i = agent->nobjs - 1; i >= 0; i--)
    if (agent->objs[i].group && strcmp(agent->objs[i].group, group) == 0)
      forget_object(agent, i);
}

/* Debuggee -> C string, swallowing any exception a toString() raises. The
   frontend asked for a description, not for the program's error state. */
static const char *debuggee_to_string(QJSCDPAgent *agent, JSValue v) {
  const char *s = JS_ToCString(agent->ctx, v);
  if (!s && JS_HasException(agent->ctx))
    JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
  return s;
}

static void free_debuggee_string(QJSCDPAgent *agent, const char *s) {
  JS_FreeCString(agent->ctx, s);
}

static const char *subtype_name(QJSCDPAgent *agent, JSValue v) {
  if (JS_IsNull(v)) return "null";
  if (JS_IsArray(v)) return "array";
  if (JS_IsError(v)) return "error";
  if (JS_IsDate(v)) return "date";
  if (JS_IsRegExp(v)) return "regexp";
  if (JS_IsMap(v)) return "map";
  if (JS_IsPromise(v)) return "promise";
  if (JS_IsProxy(v)) return "proxy";
  return NULL;
}

static void set_description(QJSCDPAgent *agent, JSValue out, JSValue v) {
  const char *s = debuggee_to_string(agent, v);
  if (s) {
    json_set_string(agent, out, "description", s);
    free_debuggee_string(agent, s);
  }
}

/* Takes a borrowed debuggee value; returns a protocol-context object. */
static JSValue to_remote_object(
    QJSCDPAgent *agent, JSValue v, const char *group) {
  JSValue o = json_object(agent);

  if (JS_IsUndefined(v)) {
    json_set_string(agent, o, "type", "undefined");
    return o;
  }
  if (JS_IsNull(v)) {
    json_set_string(agent, o, "type", "object");
    json_set_string(agent, o, "subtype", "null");
    json_set(agent, o, "value", JS_NULL);
    return o;
  }
  if (JS_IsBool(v)) {
    json_set_string(agent, o, "type", "boolean");
    json_set_bool(agent, o, "value", JS_ToBool(agent->ctx, v));
    return o;
  }
  if (JS_IsNumber(v)) {
    double d;
    JS_ToFloat64(agent->ctx, &d, v);
    json_set_string(agent, o, "type", "number");
    json_set(agent, o, "value", JS_NewFloat64(agent->jctx, d));
    set_description(agent, o, v);
    return o;
  }
  if (JS_IsString(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "string");
    json_set_string(agent, o, "value", s);
    free_debuggee_string(agent, s);
    return o;
  }
  if (JS_IsBigInt(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "bigint");
    json_set_string(agent, o, "description", s);
    free_debuggee_string(agent, s);
    return o;
  }
  if (JS_IsSymbol(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "symbol");
    json_set_string(agent, o, "description", s);
    free_debuggee_string(agent, s);
    char sid[16];
    snprintf(sid, sizeof(sid), "%d", remember_object(agent, v, group));
    json_set_string(agent, o, "objectId", sid);
    return o;
  }

  bool is_fn = JS_IsFunction(agent->ctx, v);
  json_set_string(agent, o, "type", is_fn ? "function" : "object");

  const char *sub = subtype_name(agent, v);
  if (sub) json_set_string(agent, o, "subtype", sub);

  /* className comes from the constructor rather than from toString, which for
     a plain object is "[object Object]" and tells the frontend nothing. */
  JSValue ctor = JS_GetPropertyStr(agent->ctx, v, "constructor");
  JSValue name = JS_IsObject(ctor) ? JS_GetPropertyStr(agent->ctx, ctor, "name")
                                   : JS_UNDEFINED;
  const char *cn = JS_IsString(name) ? debuggee_to_string(agent, name) : NULL;
  json_set_string(agent, o, "className", cn && *cn ? cn : "Object");
  if (cn) free_debuggee_string(agent, cn);
  JS_FreeValue(agent->ctx, name);
  JS_FreeValue(agent->ctx, ctor);

  set_description(agent, o, v);

  char id[16];
  snprintf(id, sizeof(id), "%d", remember_object(agent, v, group));
  json_set_string(agent, o, "objectId", id);
  return o;
}

/* Takes ownership of exc. */
static JSValue to_exception_details(QJSCDPAgent *agent, JSValue exc) {
  JSValue d = json_object(agent);
  json_set_int(agent, d, "exceptionId", 1);
  json_set_int(agent, d, "lineNumber", 0);
  json_set_int(agent, d, "columnNumber", 0);

  const char *text = debuggee_to_string(agent, exc);
  json_set_string(agent, d, "text", text ? text : "Uncaught");
  if (text) free_debuggee_string(agent, text);

  json_set(agent, d, "exception", to_remote_object(agent, exc, NULL));
  JS_FreeValue(agent->ctx, exc);
  return d;
}

/* ------------------------------------------------------------- scripts --- */

static Script *find_script_by_url(QJSCDPAgent *agent, const char *url) {
  if (!url) return NULL;
  for (int i = 0; i < agent->nscripts; i++)
    if (strcmp(agent->scripts[i].url, url) == 0) return &agent->scripts[i];
  return NULL;
}

static Script *find_script_by_id(QJSCDPAgent *agent, const char *script_id) {
  if (!script_id) return NULL;
  int id = atoi(script_id);
  for (int i = 0; i < agent->nscripts; i++)
    if (agent->scripts[i].id == id) return &agent->scripts[i];
  return NULL;
}

/* A breakpoint URL matches a script whose URL ends with it, so that a frontend
   that only knows "app.js" still binds against "http://localhost/app.js". */
static bool url_matches(const char *script_url, const char *wanted) {
  size_t n = strlen(script_url), m = strlen(wanted);
  return m <= n && strcmp(script_url + n - m, wanted) == 0;
}

static void announce_script(QJSCDPAgent *agent, const Script *script) {
  char id[16];
  snprintf(id, sizeof(id), "%d", script->id);

  int lines = 0;
  for (const char *p = script->source; *p; p++)
    if (*p == '\n') lines++;

  JSValue p = json_object(agent);
  json_set_string(agent, p, "scriptId", id);
  json_set_string(agent, p, "url", script->url);
  json_set_int(agent, p, "startLine", 0);
  json_set_int(agent, p, "startColumn", 0);
  json_set_int(agent, p, "endLine", lines);
  json_set_int(agent, p, "endColumn", 0);
  json_set_int(agent, p, "executionContextId", agent->exec_ctx_id);
  json_set_string(agent, p, "hash", id);
  send_event(agent, "Debugger.scriptParsed", p);
}

/* --------------------------------------------------------- breakpoints --- */

static void announce_breakpoint(
    QJSCDPAgent *agent, const Breakpoint *bp, const Script *script) {
  char bid[16], sid[16];
  snprintf(bid, sizeof(bid), "%d", bp->id);
  snprintf(sid, sizeof(sid), "%d", script->id);

  JSValue loc = json_object(agent);
  json_set_string(agent, loc, "scriptId", sid);
  json_set_int(agent, loc, "lineNumber", bp->line - 1);
  json_set_int(agent, loc, "columnNumber", bp->col > 0 ? bp->col - 1 : 0);

  JSValue p = json_object(agent);
  json_set_string(agent, p, "breakpointId", bid);
  json_set(agent, p, "location", loc);
  send_event(agent, "Debugger.breakpointResolved", p);
}

/* Binds every breakpoint that named this script's URL but had no script to
   attach to when it was set. */
static void resolve_breakpoints(QJSCDPAgent *agent, const Script *script) {
  for (int i = 0; i < agent->nbps; i++) {
    Breakpoint *bp = &agent->bps[i];
    if (bp->resolved || !url_matches(script->url, bp->url)) continue;
    bp->resolved = true;
    announce_breakpoint(agent, bp, script);
  }
}

static Breakpoint *breakpoint_at(
    QJSCDPAgent *agent, const char *file, int line) {
  if (!agent->breakpoints_active || !file) return NULL;
  for (int i = 0; i < agent->nbps; i++)
    if (agent->bps[i].line == line && url_matches(file, agent->bps[i].url))
      return &agent->bps[i];
  return NULL;
}

/* --------------------------------------------------------- call frames --- */

/* The frame's arguments, locals and captured variables, as one debuggee object
   the frontend can expand. */
static JSValue frame_scope_object(QJSCDPAgent *agent, int level) {
  JSDebugLocalVar *vars = NULL;
  int count = 0;
  if (JS_GetLocalVariablesAtLevel(agent->ctx, level, &vars, &count) < 0)
    return JS_NewObject(agent->ctx);

  JSValue scope = JS_NewObject(agent->ctx);
  for (int i = 0; i < count; i++)
    JS_SetPropertyStr(
        agent->ctx, scope, vars[i].name,
        JS_DupValue(agent->ctx, vars[i].value));
  JS_FreeLocalVariables(agent->ctx, vars, count);
  return scope;
}

static JSValue call_frames(QJSCDPAgent *agent) {
  JSValue frames = json_array(agent);
  int depth = JS_GetStackDepth(agent->ctx);
  uint32_t n = 0;

  for (int level = 0; level < depth; level++) {
    JSDebugFrameInfo info;
    if (JS_GetFrameInfoAtLevel(agent->ctx, level, &info) < 0) break;
    if (info.is_native) continue;

    const char *file = info.filename != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.filename)
                           : NULL;
    const char *func = info.func_name != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.func_name)
                           : NULL;
    Script *script = file ? find_script_by_url(agent, file) : NULL;

    char frame_id[16], script_id[16];
    snprintf(frame_id, sizeof(frame_id), "%d", level);
    snprintf(script_id, sizeof(script_id), "%d", script ? script->id : 0);

    /* CDP counts lines and columns from zero; the engine counts from one. */
    JSValue location = json_object(agent);
    json_set_string(agent, location, "scriptId", script_id);
    json_set_int(
        agent, location, "lineNumber", info.line > 0 ? info.line - 1 : 0);
    json_set_int(
        agent, location, "columnNumber", info.col > 0 ? info.col - 1 : 0);

    JSValue locals = frame_scope_object(agent, level);
    JSValue scope = json_object(agent);
    json_set_string(agent, scope, "type", "local");
    json_set(
        agent, scope, "object", to_remote_object(agent, locals, "backtrace"));
    JS_FreeValue(agent->ctx, locals);

    JSValue scope_chain = json_array(agent);
    json_set_index(agent, scope_chain, 0, scope);

    JSValue frame = json_object(agent);
    json_set_string(agent, frame, "callFrameId", frame_id);
    json_set_string(agent, frame, "functionName", func ? func : "");
    json_set(agent, frame, "location", location);
    json_set(agent, frame, "url", JS_NewString(agent->jctx, file ? file : ""));
    json_set(agent, frame, "scopeChain", scope_chain);
    json_set(agent, frame, "this", to_remote_object(agent, JS_UNDEFINED, NULL));
    json_set_index(agent, frames, n++, frame);

    if (file) JS_FreeCString(agent->ctx, file);
    if (func) JS_FreeCString(agent->ctx, func);
  }
  return frames;
}

/* ------------------------------------------------------------- pausing --- */

static void handle_message(QJSCDPAgent *agent, const char *json, size_t len);

/* Caller holds the lock. */
static char *take_message(QJSCDPAgent *agent) {
  if (agent->queue_len == 0) return NULL;
  char *msg = agent->queue[0];
  memmove(
      agent->queue, agent->queue + 1,
      sizeof(char *) * (size_t)(--agent->queue_len));
  return msg;
}

/* Stops the JS thread here and answers the frontend from inside the engine
   until it is told to continue. This is the only reason the message queue
   exists: while this loop is running the JS thread cannot return to whatever
   would otherwise be pumping it. */
static void pause_until_resumed(
    QJSCDPAgent *agent, const char *reason, int breakpoint_id) {
  agent->paused = true;
  agent->resumed = false;
  agent->pause_pending = false;
  agent->mode = MODE_RUN;

  JSValue params = json_object(agent);
  json_set(agent, params, "callFrames", call_frames(agent));
  json_set_string(agent, params, "reason", reason);
  if (breakpoint_id > 0) {
    char bid[16];
    snprintf(bid, sizeof(bid), "%d", breakpoint_id);
    JSValue hit = json_array(agent);
    json_set_index(agent, hit, 0, JS_NewString(agent->jctx, bid));
    json_set(agent, params, "hitBreakpoints", hit);
  }
  send_event(agent, "Debugger.paused", params);

  pthread_mutex_lock(&agent->lock);
  for (;;) {
    char *msg = take_message(agent);
    if (msg) {
      pthread_mutex_unlock(&agent->lock);
      handle_message(agent, msg, strlen(msg));
      free(msg);
      pthread_mutex_lock(&agent->lock);
      continue;
    }
    if (agent->resumed) break;
    pthread_cond_wait(&agent->cond, &agent->lock);
  }
  pthread_mutex_unlock(&agent->lock);

  agent->paused = false;
  send_event(agent, "Debugger.resumed", json_object(agent));
}

static int on_statement(
    JSContext *ctx, JSAtom filename, JSAtom funcname, int line, int col,
    int flags, void *opaque) {
  (void)funcname;
  (void)col;
  QJSCDPAgent *agent = opaque;

  /* Re-entrant: an evaluate issued from the pause loop runs instrumented code
     of its own, and stopping inside it would deadlock the frontend. */
  if (agent->paused || !agent->debugger_enabled) return 0;

  const char *reason = NULL;
  int breakpoint_id = 0;
  int depth = JS_GetStackDepth(ctx);

  if (agent->pause_pending) {
    reason = "other";
  } else if (flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
    reason = "debuggerStatement";
  } else if (agent->mode == MODE_PAUSE_NEXT) {
    reason = "other";
  } else if (agent->mode == MODE_STEP_INTO) {
    reason = "step";
  } else if (agent->mode == MODE_STEP_OVER && depth <= agent->step_depth) {
    reason = "step";
  } else if (agent->mode == MODE_STEP_OUT && depth < agent->step_depth) {
    reason = "step";
  }

  if (!reason && filename != JS_ATOM_NULL) {
    const char *file = JS_AtomToCString(ctx, filename);
    Breakpoint *bp = breakpoint_at(agent, file, line);
    if (bp) {
      reason = "other";
      breakpoint_id = bp->id;
    }
    JS_FreeCString(ctx, file);
  }

  if (reason) pause_until_resumed(agent, reason, breakpoint_id);
  return 0;
}

static void resume_with(QJSCDPAgent *agent, PauseMode mode) {
  agent->mode = mode;
  agent->step_depth = JS_GetStackDepth(agent->ctx);
  pthread_mutex_lock(&agent->lock);
  agent->resumed = true;
  pthread_cond_broadcast(&agent->cond);
  pthread_mutex_unlock(&agent->lock);
}

/* ------------------------------------------------------------- methods --- */

/* Both engines can already read and write JSON, so a value crosses between
   them as text rather than through a converter of our own. */
static JSValue protocol_value_to_debuggee(QJSCDPAgent *agent, JSValue v) {
  JSValue text = JS_JSONStringify(agent->jctx, v, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *s = JS_ToCStringLen(agent->jctx, &len, text);
  JSValue out = JS_UNDEFINED;
  if (s) {
    out = JS_ParseJSON(agent->ctx, s, len, "<argument>");
    if (JS_IsException(out)) {
      JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
      out = JS_UNDEFINED;
    }
    JS_FreeCString(agent->jctx, s);
  }
  JS_FreeValue(agent->jctx, text);
  return out;
}

/* Takes ownership of value. Answers with the CDP shape shared by
   Runtime.evaluate and Debugger.evaluateOnCallFrame. */
static void send_evaluation(
    QJSCDPAgent *agent, int id, JSValue value, const char *group) {
  JSValue result = json_object(agent);
  if (JS_IsException(value)) {
    JSValue exc = JS_GetException(agent->ctx);
    json_set(agent, result, "result", to_remote_object(agent, exc, group));
    json_set(
        agent, result, "exceptionDetails",
        to_exception_details(agent, JS_DupValue(agent->ctx, exc)));
    JS_FreeValue(agent->ctx, exc);
  } else {
    json_set(agent, result, "result", to_remote_object(agent, value, group));
  }
  JS_FreeValue(agent->ctx, value);
  send_result(agent, id, result);
}

static void send_properties(QJSCDPAgent *agent, int id, JSValue params) {
  const char *object_id = json_get_string(agent, params, "objectId");
  RemoteObject *remote = find_object(agent, object_id);
  if (object_id) JS_FreeCString(agent->jctx, object_id);
  if (!remote) {
    send_error(agent, id, "No object found for the given objectId");
    return;
  }

  JSPropertyEnum *props = NULL;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(
          agent->ctx, &props, &count, remote->value,
          JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    send_error(agent, id, "Could not read the object's properties");
    return;
  }

  JSValue list = json_array(agent);
  for (uint32_t i = 0; i < count; i++) {
    const char *name = JS_AtomToCString(agent->ctx, props[i].atom);
    JSValue value = JS_GetProperty(agent->ctx, remote->value, props[i].atom);
    if (JS_IsException(value)) {
      JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
      value = JS_UNDEFINED;
    }
    JSValue entry = json_object(agent);
    json_set_string(agent, entry, "name", name ? name : "");
    json_set(agent, entry, "value", to_remote_object(agent, value, NULL));
    json_set_bool(agent, entry, "writable", true);
    json_set_bool(agent, entry, "configurable", true);
    json_set_bool(agent, entry, "enumerable", true);
    json_set_bool(agent, entry, "isOwn", true);
    json_set_index(agent, list, i, entry);
    JS_FreeValue(agent->ctx, value);
    if (name) JS_FreeCString(agent->ctx, name);
  }
  JS_FreePropertyEnum(agent->ctx, props, count);

  JSValue result = json_object(agent);
  json_set(agent, result, "result", list);
  send_result(agent, id, result);
}

static void set_breakpoint_by_url(QJSCDPAgent *agent, int id, JSValue params) {
  const char *url = json_get_string(agent, params, "url");
  if (!url) {
    send_error(agent, id, "Debugger.setBreakpointByUrl needs a url");
    return;
  }
  const char *condition = json_get_string(agent, params, "condition");

  agent->bps = realloc(agent->bps, sizeof(Breakpoint) * (agent->nbps + 1));
  Breakpoint *bp = &agent->bps[agent->nbps++];
  bp->id = ++agent->next_bp_id;
  bp->url = strdup(url);
  bp->condition = condition && *condition ? strdup(condition) : NULL;
  bp->line = json_get_int(agent, params, "lineNumber", 0) + 1;
  bp->col = json_get_int(agent, params, "columnNumber", 0) + 1;
  bp->resolved = false;

  char bid[16];
  snprintf(bid, sizeof(bid), "%d", bp->id);
  JSValue locations = json_array(agent);

  Script *script = NULL;
  for (int i = 0; i < agent->nscripts; i++)
    if (url_matches(agent->scripts[i].url, url)) script = &agent->scripts[i];

  if (script) {
    bp->resolved = true;
    char sid[16];
    snprintf(sid, sizeof(sid), "%d", script->id);
    JSValue loc = json_object(agent);
    json_set_string(agent, loc, "scriptId", sid);
    json_set_int(agent, loc, "lineNumber", bp->line - 1);
    json_set_int(agent, loc, "columnNumber", bp->col - 1);
    json_set_index(agent, locations, 0, loc);
  }

  JSValue result = json_object(agent);
  json_set_string(agent, result, "breakpointId", bid);
  json_set(agent, result, "locations", locations);
  send_result(agent, id, result);

  JS_FreeCString(agent->jctx, url);
  if (condition) JS_FreeCString(agent->jctx, condition);
}

static void remove_breakpoint(QJSCDPAgent *agent, JSValue params) {
  const char *bid = json_get_string(agent, params, "breakpointId");
  if (!bid) return;
  int wanted = atoi(bid);
  for (int i = 0; i < agent->nbps; i++) {
    if (agent->bps[i].id != wanted) continue;
    free(agent->bps[i].url);
    free(agent->bps[i].condition);
    agent->bps[i] = agent->bps[--agent->nbps];
    break;
  }
  JS_FreeCString(agent->jctx, bid);
}

/* Writes a local in a frame that is stopped, so that the frontend's "edit
   value" reaches the live frame rather than a copy of it. */
static void set_variable_value(QJSCDPAgent *agent, int id, JSValue params) {
  const char *frame_id = json_get_string(agent, params, "callFrameId");
  const char *name = json_get_string(agent, params, "variableName");
  if (!frame_id || !name) {
    send_error(
        agent, id, "Debugger.setVariableValue needs a call frame and a name");
    goto done;
  }

  JSValue arg = json_get(agent, params, "newValue");
  JSValue wanted = json_get(agent, arg, "value");
  JSValue value = protocol_value_to_debuggee(agent, wanted);
  JS_FreeValue(agent->jctx, wanted);
  JS_FreeValue(agent->jctx, arg);

  int rc = JS_SetVariableAtLevel(agent->ctx, atoi(frame_id), name, value);
  if (rc == 0)
    send_empty_result(agent, id);
  else if (rc == -2)
    send_error(agent, id, "That binding is a constant");
  else
    send_error(agent, id, "No such variable in that call frame");

done:
  if (frame_id) JS_FreeCString(agent->jctx, frame_id);
  if (name) JS_FreeCString(agent->jctx, name);
}

/* ------------------------------------------------------------ dispatch --- */

static bool is(const char *method, const char *name) {
  return strcmp(method, name) == 0;
}

static void dispatch(
    QJSCDPAgent *agent, int id, const char *method, JSValue params) {
  /* --- Runtime --- */
  if (is(method, "Runtime.enable")) {
    agent->runtime_enabled = true;
    JSValue ctx_info = json_object(agent);
    json_set_int(agent, ctx_info, "id", agent->exec_ctx_id);
    json_set_string(agent, ctx_info, "origin", "");
    json_set_string(agent, ctx_info, "name", "QuickJS");
    JSValue p = json_object(agent);
    json_set(agent, p, "context", ctx_info);
    send_event(agent, "Runtime.executionContextCreated", p);
    send_empty_result(agent, id);

  } else if (is(method, "Runtime.disable")) {
    agent->runtime_enabled = false;
    send_empty_result(agent, id);

  } else if (is(method, "Runtime.evaluate")) {
    const char *expr = json_get_string(agent, params, "expression");
    const char *group = json_get_string(agent, params, "objectGroup");
    JSValue v = expr ? JS_Eval(
                           agent->ctx, expr, strlen(expr), "<evaluate>",
                           JS_EVAL_TYPE_GLOBAL)
                     : JS_UNDEFINED;
    send_evaluation(agent, id, v, group);
    if (expr) JS_FreeCString(agent->jctx, expr);
    if (group) JS_FreeCString(agent->jctx, group);

  } else if (is(method, "Runtime.getProperties")) {
    send_properties(agent, id, params);

  } else if (is(method, "Runtime.releaseObject")) {
    const char *object_id = json_get_string(agent, params, "objectId");
    release_object(agent, object_id);
    if (object_id) JS_FreeCString(agent->jctx, object_id);
    send_empty_result(agent, id);

  } else if (is(method, "Runtime.releaseObjectGroup")) {
    const char *group = json_get_string(agent, params, "objectGroup");
    release_object_group(agent, group);
    if (group) JS_FreeCString(agent->jctx, group);
    send_empty_result(agent, id);

  } else if (is(method, "Runtime.getHeapUsage")) {
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(JS_GetRuntime(agent->ctx), &usage);
    JSValue result = json_object(agent);
    json_set_int(agent, result, "usedSize", usage.memory_used_size);
    json_set_int(agent, result, "totalSize", usage.malloc_size);
    send_result(agent, id, result);

  } else if (
      is(method, "Runtime.discardConsoleEntries") ||
      is(method, "Runtime.setAsyncCallStackDepth") ||
      is(method, "Runtime.setCustomObjectFormatterEnabled") ||
      is(method, "Runtime.setMaxCallStackSizeToCapture")) {
    send_empty_result(agent, id);

    /* --- Debugger --- */
  } else if (is(method, "Debugger.enable")) {
    agent->debugger_enabled = true;
    JS_SetDebugTraceArmed(agent->ctx, true);
    for (int i = 0; i < agent->nscripts; i++)
      announce_script(agent, &agent->scripts[i]);
    JSValue result = json_object(agent);
    json_set_string(agent, result, "debuggerId", "quickjs");
    send_result(agent, id, result);

  } else if (is(method, "Debugger.disable")) {
    agent->debugger_enabled = false;
    /* Disarmed rather than uninstalled: uninstalling the handler would
       permanently un-instrument everything parsed afterwards. */
    JS_SetDebugTraceArmed(agent->ctx, false);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.setBreakpointByUrl")) {
    set_breakpoint_by_url(agent, id, params);

  } else if (is(method, "Debugger.removeBreakpoint")) {
    remove_breakpoint(agent, params);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.setBreakpointsActive")) {
    agent->breakpoints_active = json_get_bool(agent, params, "active", true);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.resume")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_RUN);

  } else if (is(method, "Debugger.stepOver")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_OVER);

  } else if (is(method, "Debugger.stepInto")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_INTO);

  } else if (is(method, "Debugger.stepOut")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_OUT);

  } else if (is(method, "Debugger.pause")) {
    agent->pause_pending = true;
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.evaluateOnCallFrame")) {
    const char *frame_id = json_get_string(agent, params, "callFrameId");
    const char *expr = json_get_string(agent, params, "expression");
    const char *group = json_get_string(agent, params, "objectGroup");
    JSValue v = (frame_id && expr) ? JS_EvalInStackFrame(
                                         agent->ctx, atoi(frame_id), expr,
                                         strlen(expr), "<evaluate>")
                                   : JS_UNDEFINED;
    send_evaluation(agent, id, v, group);
    if (frame_id) JS_FreeCString(agent->jctx, frame_id);
    if (expr) JS_FreeCString(agent->jctx, expr);
    if (group) JS_FreeCString(agent->jctx, group);

  } else if (is(method, "Debugger.setVariableValue")) {
    set_variable_value(agent, id, params);

  } else if (is(method, "Debugger.getScriptSource")) {
    const char *script_id = json_get_string(agent, params, "scriptId");
    Script *script = find_script_by_id(agent, script_id);
    if (script_id) JS_FreeCString(agent->jctx, script_id);
    if (!script) {
      send_error(agent, id, "No script with that scriptId");
    } else {
      JSValue result = json_object(agent);
      json_set_string(agent, result, "scriptSource", script->source);
      send_result(agent, id, result);
    }

  } else if (
      is(method, "Debugger.setPauseOnExceptions") ||
      is(method, "Debugger.setAsyncCallStackDepth") ||
      is(method, "Debugger.setBlackboxPatterns") ||
      is(method, "Debugger.setBlackboxedRanges") ||
      is(method, "Debugger.setSkipAllPauses")) {
    send_empty_result(agent, id);

  } else {
    send_error(agent, id, "Not supported by this engine");
  }
}

static void handle_message(QJSCDPAgent *agent, const char *json, size_t len) {
  JSValue msg = JS_ParseJSON(agent->jctx, json, len, "<cdp>");
  if (JS_IsException(msg)) {
    JS_FreeValue(agent->jctx, JS_GetException(agent->jctx));
    JS_FreeValue(agent->jctx, msg);
    return;
  }

  int id = json_get_int(agent, msg, "id", 0);
  const char *method = json_get_string(agent, msg, "method");
  JSValue params = json_get(agent, msg, "params");
  if (!JS_IsObject(params)) {
    JS_FreeValue(agent->jctx, params);
    params = json_object(agent);
  }

  if (method)
    dispatch(agent, id, method, params);
  else
    send_error(agent, id, "Message had no method");

  if (method) JS_FreeCString(agent->jctx, method);
  JS_FreeValue(agent->jctx, params);
  JS_FreeValue(agent->jctx, msg);
}

/* -------------------------------------------------------- the C API ------ */

QJSCDPAgent *qjs_cdp_new(
    JSContext *ctx, int execution_context_id, QJSCDPSendFunc *send,
    void *send_opaque) {
  QJSCDPAgent *agent = calloc(1, sizeof(*agent));
  agent->ctx = ctx;
  agent->exec_ctx_id = execution_context_id;
  agent->send = send;
  agent->send_opaque = send_opaque;
  agent->breakpoints_active = true;
  agent->mode = MODE_RUN;
  pthread_mutex_init(&agent->lock, NULL);
  pthread_cond_init(&agent->cond, NULL);

  /* Base objects and JSON, nothing else: this runtime never runs a program. */
  agent->jrt = JS_NewRuntime();
  agent->jctx = JS_NewContextRaw(agent->jrt);
  JS_AddIntrinsicBaseObjects(agent->jctx);
  JS_AddIntrinsicJSON(agent->jctx);

  JS_SetDebugTraceHandler(ctx, on_statement, agent);
  JS_SetDebugTraceArmed(ctx, false);
  return agent;
}

void qjs_cdp_free(QJSCDPAgent *agent) {
  if (!agent) return;
  JS_SetDebugTraceHandler(agent->ctx, NULL, NULL);

  for (int i = 0; i < agent->nobjs; i++) {
    JS_FreeValue(agent->ctx, agent->objs[i].value);
    free(agent->objs[i].group);
  }
  for (int i = 0; i < agent->nscripts; i++) {
    free(agent->scripts[i].url);
    free(agent->scripts[i].source);
  }
  for (int i = 0; i < agent->nbps; i++) {
    free(agent->bps[i].url);
    free(agent->bps[i].condition);
  }
  for (int i = 0; i < agent->queue_len; i++) free(agent->queue[i]);

  free(agent->objs);
  free(agent->scripts);
  free(agent->bps);
  free(agent->queue);

  JS_FreeContext(agent->jctx);
  JS_FreeRuntime(agent->jrt);
  pthread_cond_destroy(&agent->cond);
  pthread_mutex_destroy(&agent->lock);
  free(agent);
}

void qjs_cdp_send_message(QJSCDPAgent *agent, const char *json, size_t len) {
  char *copy = malloc(len + 1);
  memcpy(copy, json, len);
  copy[len] = '\0';

  pthread_mutex_lock(&agent->lock);
  if (agent->queue_len == agent->queue_cap) {
    agent->queue_cap = agent->queue_cap ? agent->queue_cap * 2 : 8;
    agent->queue =
        realloc(agent->queue, sizeof(char *) * (size_t)agent->queue_cap);
  }
  agent->queue[agent->queue_len++] = copy;
  pthread_cond_broadcast(&agent->cond);
  pthread_mutex_unlock(&agent->lock);
}

void qjs_cdp_poll(QJSCDPAgent *agent) {
  for (;;) {
    pthread_mutex_lock(&agent->lock);
    char *msg = take_message(agent);
    pthread_mutex_unlock(&agent->lock);
    if (!msg) return;
    handle_message(agent, msg, strlen(msg));
    free(msg);
  }
}

void qjs_cdp_script_loaded(
    QJSCDPAgent *agent, const char *url, const char *source) {
  agent->scripts =
      realloc(agent->scripts, sizeof(Script) * (size_t)(agent->nscripts + 1));
  Script *script = &agent->scripts[agent->nscripts++];
  script->id = agent->nscripts;
  script->url = strdup(url ? url : "");
  script->source = strdup(source ? source : "");

  if (agent->debugger_enabled) {
    announce_script(agent, script);
    resolve_breakpoints(agent, script);
  }
}

void qjs_cdp_pause(QJSCDPAgent *agent) {
  agent->pause_pending = true;
}
