#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/middleware_registration.h"
#include "ruvia/web/detail/worker_state.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"

namespace ruvia::detail {

// One canonical spelling is shared by application, test_app and route_table.
// router_impl receives the already-normalized registrations from the public
// configuration facades.
// A trailing slash does not create a different path scope; accepting both
// spellings at one layer and not another makes duplicate fallback handlers
// order-dependent and lets tests exercise a different route graph.
// "/" (or any all-slash spelling) is rejected: it would scope every path and
// silently shadow or duplicate the unscoped registration, which is the only
// app-wide spelling.
[[nodiscard]] inline std::string_view normalize_fallback_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    if ((prefix.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    if (prefix == "/") {
        throw std::invalid_argument("fallback prefix must not be '/'; register app-wide without a prefix");
    }
    return prefix;
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. `prefix` is normalized (see normalize_fallback_prefix)
// or empty for an unscoped registration, which scopes everything.
// A prefix matches on whole path segments only, so "/api" scopes "/api" and
// "/api/x" but never "/apix". Segments compare in the
// canonical form route matching and route parameters use (uri_paths_equivalent):
// split on raw '/' and percent-decoded, so "/%61pi/x" is under "/api" exactly
// when it routes like "/api/x", while an encoded "%2F" never acts as a segment
// boundary.
[[nodiscard]] inline bool path_is_under_prefix(
    std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty()) {
        return true;
    }
    if (path.empty() || path.front() != '/') {
        return false;
    }
    std::size_t path_cursor = 1;
    std::size_t prefix_cursor = 1;
    for (;;) {
        const auto prefix_slash = prefix.find('/', prefix_cursor);
        const auto prefix_end = prefix_slash == std::string_view::npos ? prefix.size() : prefix_slash;
        const auto path_slash = path.find('/', path_cursor);
        const auto path_end = path_slash == std::string_view::npos ? path.size() : path_slash;
        if (!uri_paths_equivalent(path.substr(path_cursor, path_end - path_cursor),
                prefix.substr(prefix_cursor, prefix_end - prefix_cursor))) {
            return false;
        }
        if (prefix_end == prefix.size()) {
            // path_end is a '/' or the end of the path: a segment boundary.
            return true;
        }
        if (path_end == path.size()) {
            return false;
        }
        prefix_cursor = prefix_end + 1;
        path_cursor = path_end + 1;
    }
}

// Two normalized prefixes that scope the same request paths, such as "/api"
// and "/%61pi", are one registration.
[[nodiscard]] inline bool fallback_prefixes_equivalent(std::string_view left, std::string_view right) noexcept {
    return path_is_under_prefix(left, right) && path_is_under_prefix(right, left);
}

// Segment count of a normalized prefix. A scope nested inside another always
// has more segments, whatever either one's escaping.
[[nodiscard]] inline std::size_t fallback_prefix_depth(std::string_view prefix) noexcept {
    std::size_t depth = 0;
    for (const auto c : prefix) {
        depth += c == '/' ? 1U : 0U;
    }
    return depth;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class router;

template <typename callback_type>
using callback_registration = std::unique_ptr<callback_type>;

// Pin configured callables before entering the application lock: publication
// only swaps owning pointers, and router views borrow an address-stable owner.
template <typename signature_type>
[[nodiscard]] callback_registration<move_only_function<signature_type>> own_callback(
    move_only_function<signature_type>&& callback) {
    if (!callback) {
        return {};
    }
    return std::make_unique<move_only_function<signature_type>>(std::move(callback));
}

template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_scoped_app_middleware(
    const middleware_scope_options& options, args_types&&... args) {
    static_assert(!middleware_runs_on_unmatched_requests<middleware_type>(),
        "a middleware declaring ruvia_runs_on_unmatched_requests cannot be path-scoped with "
        "use_at(); register it app-wide with use<T>()");
    const auto normalized = normalize_fallback_prefix(options.prefix_.view());
    return make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...)
        .scoped_to(normalized);
}

template <typename state_type>
[[nodiscard]] worker_state_definition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "use_worker_state<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return worker_state_definition::make<state_type>([] { return state_type(); });
}

// application and test_app own this same dispatch configuration. Their lifecycle guards
// remain outside the component; validation, callback ownership, registration
// storage and router binding have one implementation. Bound callback views
// borrow this immovable owner until its routers have retired.
class app_configuration final {
public:
    app_configuration();

    app_configuration(const app_configuration&) = delete;
    app_configuration& operator=(const app_configuration&) = delete;
    app_configuration(app_configuration&&) = delete;
    app_configuration& operator=(app_configuration&&) = delete;

    void add_middleware(owned_middleware_descriptor&& descriptor);
    void add_worker_state(worker_state_definition&& definition);
    void on_error(callback_registration<http_error_handler>&& handler);
    void on_error(borrowed_text prefix, callback_registration<http_error_handler>&& handler);
    // Returns the cleared handler so the caller destroys it after releasing its lock.
    [[nodiscard]] callback_registration<http_error_handler> on_error(std::nullptr_t) noexcept;
    void on_not_found(callback_registration<http_not_found_handler>&& handler);
    void on_not_found(borrowed_text prefix, callback_registration<http_not_found_handler>&& handler);
    [[nodiscard]] callback_registration<http_not_found_handler> on_not_found(std::nullptr_t) noexcept;

    [[nodiscard]] std::span<const worker_state_definition> worker_states() const noexcept {
        return worker_states_;
    }

    void apply(router& routes) const;

private:
    callback_registration<http_error_handler> error_handler_;
    callback_registration<http_not_found_handler> not_found_handler_;
    std::vector<std::pair<std::string, callback_registration<http_error_handler>>> prefix_error_handlers_;
    std::vector<std::pair<std::string, callback_registration<http_not_found_handler>>> prefix_not_found_handlers_;
    std::vector<owned_middleware_descriptor> middlewares_;
    std::vector<worker_state_definition> worker_states_;
};

}  // namespace ruvia::detail
