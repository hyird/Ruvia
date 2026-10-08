#pragma once

namespace ruvia {

class App;

namespace detail {

struct AppState;

void runApp(App& app, AppState& state);

}  // namespace detail
}  // namespace ruvia
