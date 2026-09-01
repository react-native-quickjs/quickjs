/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickJSInspector.h"

#include <utility>

#include "QuickJSRuntime.h"

using namespace facebook::react;
using namespace facebook::react::jsinspector_modern;

namespace qjs {

namespace {

/// One debugging session. Requests arrive on the inspector thread and are
/// queued; the agent handles them on the JS thread, or from inside the pause
/// loop when execution is stopped.
class SessionAgent : public RuntimeAgentDelegate {
 public:
  SessionAgent(QJSCDPAgent *agent, RuntimeExecutor executor)
      : agent_(agent), executor_(std::move(executor)) {}

  bool handleRequest(const cdp::PreparsedRequest &req) override {
    const std::string json = req.toJson();
    qjs_cdp_send_message(agent_, json.c_str(), json.size());
    // Nothing happens until someone runs the queue on the JS thread. While
    // paused this task will not run at all, which is correct: the pause loop is
    // draining the same queue.
    executor_(
        [agent = agent_](facebook::jsi::Runtime &) { qjs_cdp_poll(agent); });
    return true;
  }

 private:
  QJSCDPAgent *agent_;
  RuntimeExecutor executor_;
};

}  // namespace

QuickJSInspectorDelegate::QuickJSInspectorDelegate(JSContext *ctx)
    : FallbackRuntimeTargetDelegate("QuickJS") {
  agent_ =
      qjs_cdp_new(ctx, 1, &QuickJSInspectorDelegate::sendToFrontends, this);
}

QuickJSInspectorDelegate::~QuickJSInspectorDelegate() {
  qjs_cdp_free(agent_);
}

void QuickJSInspectorDelegate::sendToFrontends(
    void *opaque, const char *json, size_t len) {
  auto *self = static_cast<QuickJSInspectorDelegate *>(opaque);
  const std::string_view message(json, len);
  std::lock_guard<std::mutex> guard(self->lock_);
  for (auto &channel : self->channels_) channel(message);
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
  (void)executionContextDescription;
  {
    std::lock_guard<std::mutex> guard(lock_);
    channels_.push_back(channel);
  }
  return std::make_unique<SessionAgent>(agent_, std::move(runtimeExecutor));
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
