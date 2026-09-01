/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSInspector.h"

#include <folly/json.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "QuickJSRuntime.h"

using namespace facebook::react;
using namespace facebook::react::jsinspector_modern;

namespace qjs {

/// One debugging session. Requests arrive on the inspector thread and are
/// queued; the agent handles them on the JS thread, or from inside the pause
/// loop when execution is stopped.
class InspectorSession : public RuntimeAgentDelegate {
 public:
  InspectorSession(
      QuickJSInspectorDelegate &owner, QJSCDPAgent *agent,
      FrontendChannel channel, RuntimeExecutor executor, bool debuggerEnabled)
      : owner_(owner),
        agent_(agent),
        channel_(std::move(channel)),
        executor_(std::move(executor)),
        debuggerEnabled_(debuggerEnabled) {}

  ~InspectorSession() override {
    owner_.forget(this);
  }

  bool handleRequest(const cdp::PreparsedRequest &req) override {
    if (!qjs_cdp_handles(req.method.c_str())) {
      return false;
    }
    // Enabling a domain is per session, not per runtime: one frontend can be
    // watching the debugger while another is not, and sending Debugger events
    // to the second is a message it never asked for.
    if (req.method == "Debugger.enable") {
      debuggerEnabled_ = true;
      owner_.debuggerEnabledChanged();
    } else if (req.method == "Debugger.disable") {
      debuggerEnabled_ = false;
      owner_.debuggerEnabledChanged();
    }
    const std::string json = req.toJson();
    qjs_cdp_send_message(agent_, this, json.c_str(), json.size());
    // Nothing happens until someone runs the queue on the JS thread. While
    // paused this task will not run at all, which is correct: the pause loop is
    // draining the same queue.
    executor_(
        [agent = agent_](facebook::jsi::Runtime &) { qjs_cdp_poll(agent); });
    return true;
  }

  /// Handed to the agent that replaces this one when the runtime is reloaded.
  std::unique_ptr<ExportedState> getExportedState() override;

  bool debuggerEnabled() const {
    return debuggerEnabled_;
  }

  void deliver(std::string_view message) const {
    if (!debuggerEnabled_ &&
        message.find("\"method\":\"Debugger.") != std::string_view::npos) {
      return;
    }
    channel_(message);
  }

 private:
  QuickJSInspectorDelegate &owner_;
  QJSCDPAgent *agent_;
  FrontendChannel channel_;
  RuntimeExecutor executor_;
  bool debuggerEnabled_;
};

namespace {

/// The debugger state that outlives one runtime: whether it was enabled, and
/// the breakpoints the frontend believes it has set.
class CarriedState : public RuntimeAgentDelegate::ExportedState {
 public:
  explicit CarriedState(std::string json) : json_(std::move(json)) {}

  const std::string &json() const {
    return json_;
  }

 private:
  std::string json_;
};

/// A captured stack, kept as the JSON the frontend will eventually be given.
/// Capture happens on the JS thread while the frames are alive; serialization
/// can happen later, and by then they are gone.
class CapturedStack : public StackTrace {
 public:
  explicit CapturedStack(std::string json) : json_(std::move(json)) {}

  const std::string &json() const {
    return json_;
  }

 private:
  std::string json_;
};

}  // namespace

namespace {

const char *consoleTypeName(ConsoleAPIType type) {
  switch (type) {
    case ConsoleAPIType::kLog:
      return "log";
    case ConsoleAPIType::kDebug:
      return "debug";
    case ConsoleAPIType::kInfo:
      return "info";
    case ConsoleAPIType::kError:
      return "error";
    case ConsoleAPIType::kWarning:
      return "warning";
    case ConsoleAPIType::kDir:
      return "dir";
    case ConsoleAPIType::kDirXML:
      return "dirxml";
    case ConsoleAPIType::kTable:
      return "table";
    case ConsoleAPIType::kTrace:
      return "trace";
    case ConsoleAPIType::kStartGroup:
      return "startGroup";
    case ConsoleAPIType::kStartGroupCollapsed:
      return "startGroupCollapsed";
    case ConsoleAPIType::kEndGroup:
      return "endGroup";
    case ConsoleAPIType::kClear:
      return "clear";
    case ConsoleAPIType::kAssert:
      return "assert";
    case ConsoleAPIType::kTimeEnd:
      return "timeEnd";
    case ConsoleAPIType::kCount:
      return "count";
    default:
      return "log";
  }
}

}  // namespace

bool QuickJSInspectorDelegate::supportsConsole() const {
  // True, and that is the switch: with it React Native stops using its own
  // string-only fallback and routes console calls through here, where each
  // argument becomes a real remote object the frontend can expand.
  return true;
}

void QuickJSInspectorDelegate::addConsoleMessage(
    facebook::jsi::Runtime &runtime, ConsoleMessage message) {
  auto &quickjs = dynamic_cast<QuickJSRuntime &>(runtime);

  // toJSValue borrows -- the jsi::Value still owns the reference -- which is
  // what the agent wants, since it only reads each argument to describe it.
  std::vector<JSValue> args;
  args.reserve(message.args.size());
  for (const facebook::jsi::Value &arg : message.args) {
    args.push_back(quickjs.toJSValue(arg));
  }

  qjs_cdp_console_message(
      agent_, consoleTypeName(message.type), args.data(), (int)args.size());
}

std::unique_ptr<RuntimeAgentDelegate::ExportedState>
InspectorSession::getExportedState() {
  char *json = qjs_cdp_export_state(agent_);
  if (!json) {
    return std::make_unique<ExportedState>();
  }
  auto carried = std::make_unique<CarriedState>(std::string(json));
  free(json);
  return carried;
}

std::unique_ptr<StackTrace> QuickJSInspectorDelegate::captureStackTrace(
    facebook::jsi::Runtime &runtime, size_t framesToSkip) {
  (void)runtime;
  char *json = qjs_cdp_capture_stack_trace(agent_, (int)framesToSkip);
  if (!json) {
    return StackTrace::empty();
  }
  auto captured = std::make_unique<CapturedStack>(std::string(json));
  free(json);
  return captured;
}

std::optional<folly::dynamic> QuickJSInspectorDelegate::serializeStackTrace(
    const StackTrace &stackTrace) {
  const auto *captured = dynamic_cast<const CapturedStack *>(&stackTrace);
  if (!captured) {
    return std::nullopt;
  }
  return folly::parseJson(captured->json());
}

QuickJSInspectorDelegate::QuickJSInspectorDelegate(JSContext *ctx)
    : FallbackRuntimeTargetDelegate("QuickJS") {
  agent_ = qjs_cdp_new(ctx, 1, &QuickJSInspectorDelegate::deliver, this);
}

QuickJSInspectorDelegate::~QuickJSInspectorDelegate() {
  qjs_cdp_free(agent_);
}

void QuickJSInspectorDelegate::deliver(
    void *opaque, void *session, const char *json, size_t len) {
  auto *self = static_cast<QuickJSInspectorDelegate *>(opaque);
  const std::string_view message(json, len);
  std::lock_guard<std::mutex> guard(self->lock_);

  // A reply belongs to the session that asked and to no other; an event has no
  // session and belongs to all of them.
  for (auto *live : self->sessions_) {
    if (session == nullptr || live == session) {
      live->deliver(message);
    }
  }
}

std::unique_ptr<RuntimeAgentDelegate>
QuickJSInspectorDelegate::createAgentDelegate(
    FrontendChannel channel, SessionState &sessionState,
    std::unique_ptr<RuntimeAgentDelegate::ExportedState>
        previouslyExportedState,
    const ExecutionContextDescription &executionContextDescription,
    RuntimeExecutor runtimeExecutor) {
  (void)sessionState;

  // A reload replaces the runtime without telling the frontend, so the
  // breakpoints it set have to be carried over to the agent that takes over.
  if (auto *carried =
          dynamic_cast<CarriedState *>(previouslyExportedState.get())) {
    qjs_cdp_import_state(agent_, carried->json().c_str());
  }

  // React Native assigns the execution context id, and the frontend addresses
  // requests to it by number.
  qjs_cdp_set_execution_context(agent_, executionContextDescription.id);

  // A reload replaces the agent but not the frontend, so a session that had
  // the debugger enabled before still has it enabled after.
  auto session = std::make_unique<InspectorSession>(
      *this, agent_, std::move(channel), std::move(runtimeExecutor),
      qjs_cdp_debugger_enabled(agent_));
  {
    std::lock_guard<std::mutex> guard(lock_);
    sessions_.push_back(session.get());
  }
  return session;
}

void QuickJSInspectorDelegate::forget(InspectorSession *session) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), session),
        sessions_.end());
  }
  debuggerEnabledChanged();
}

/// The debugger is on for the runtime when it is on for anybody. One frontend
/// turning it off must not stop the statement traps another is relying on.
void QuickJSInspectorDelegate::debuggerEnabledChanged() {
  bool any = false;
  {
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto *session : sessions_) {
      any = any || session->debuggerEnabled();
    }
  }
  qjs_cdp_set_debugger_enabled(agent_, any);
}

void QuickJSInspectorDelegate::scriptLoaded(
    const std::string &url, const std::string &source) {
  qjs_cdp_script_loaded(agent_, url.c_str(), source.c_str());
}

QuickJSInspectorRuntime::QuickJSInspectorRuntime(
    std::unique_ptr<facebook::jsi::Runtime> runtime)
    : runtime_(std::move(runtime)) {
  auto &quickjs = dynamic_cast<QuickJSRuntime &>(*runtime_);
  delegate_ = std::make_unique<QuickJSInspectorDelegate>(quickjs.context());
  quickjs.setScriptEvaluatedHook(
      [this](const std::string &url, const std::string &source) {
        delegate_->scriptLoaded(url, source);
      });
}

facebook::jsi::Runtime &QuickJSInspectorRuntime::getRuntime() noexcept {
  return *runtime_;
}

RuntimeTargetDelegate &QuickJSInspectorRuntime::getRuntimeTargetDelegate() {
  return *delegate_;
}

}  // namespace qjs
