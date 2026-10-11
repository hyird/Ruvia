#pragma once

namespace ruvia {

class application;

namespace detail {

struct app_state;

void run_app(application& app, app_state& state_value);
// The one stop chain for a published runtime: closes admission on the acceptor
// and every worker. The caller holds state_value.mutex_; run_app owns retirement.
void request_runtime_stop(app_state& state_value) noexcept;

}  // namespace detail
}  // namespace ruvia
