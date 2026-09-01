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
      FrontendChannel channel, RuntimeExecutor executor)
      : owner_(owner),
        agent_(agent),
        channel_(std::move(channel)),
        executor_(std::move(executor)) {}

  ~InspectorSession() override {
    owner_.forget(this);
  }

  bool handleRequest(const cdp::PreparsedRequest &req) override {
    if (!qjs_cdp_handles(req.method.c_str())) {
      return false;
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

  void deliver(std::string_view message) const {
    channel_(message);
  }

 private:
  QuickJSInspectorDelegate &owner_;
  QJSCDPAgent *agent_;
  FrontendChannel channel_;
  RuntimeExecutor executor_;
};

namespace {

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
  (void)previouslyExportedState;

  // React Native assigns the execution context id, and the frontend addresses
  // requests to it by number.
  qjs_cdp_set_execution_context(agent_, executionContextDescription.id);

  auto session = std::make_unique<InspectorSession>(
      *this, agent_, std::move(channel), std::move(runtimeExecutor));
  {
    std::lock_guard<std::mutex> guard(lock_);
    sessions_.push_back(session.get());
  }
  return session;
}

void QuickJSInspectorDelegate::forget(InspectorSession *session) {
  std::lock_guard<std::mutex> guard(lock_);
  sessions_.erase(
      std::remove(sessions_.begin(), sessions_.end(), session),
      sessions_.end());
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
