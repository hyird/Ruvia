#pragma once

namespace ruvia {

class application;

namespace detail {

struct app_state;

void run_app(application& app, app_state& state_value);

}  // namespace detail
}  // namespace ruvia
