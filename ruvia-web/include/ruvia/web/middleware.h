#pragma once

#include <string>
#include <type_traits>

namespace ruvia {

template <typename t_type>
class task;
class context;
class next;
class http_response;

struct middleware_scope_options final {
    std::string prefix_{};
};

namespace detail {

template <typename middleware_type>
concept void_handle_middleware =
    requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::handle); };

template <typename middleware_type>
concept response_handle_middleware = requires {
    static_cast<task<http_response> (middleware_type::*)(context&, next&)>(&middleware_type::handle);
};

}  // namespace detail

// A middleware is any final class with a public
//     task<void> handle(context&, next&)
// or
//     task<http_response> handle(context&, next&)
// member. It derives from nothing: the framework composes it through per-type
// entry points and reads its route properties from static members.
//
// application::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs controller.h, testing.h, or
// detail/middleware/middleware_registration.h. This header stays free of
// context so app.h does not pull it in.
template <typename middleware_type>
concept middleware = std::is_class_v<middleware_type> && std::is_final_v<middleware_type> &&
                     (detail::void_handle_middleware<middleware_type> ||
                         detail::response_handle_middleware<middleware_type>);

}  // namespace ruvia
