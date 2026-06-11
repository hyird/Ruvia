#pragma once

#include <memory>
#include <stdexcept>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"

namespace ruvia {

namespace detail {

class HttpServer;
class RouteTable;
class RouterImpl;

}  // namespace detail

template <typename ControllerT>
class Controller;

class Next final {
public:
    using Invoke = Task<HttpResponse> (*)(void*, Context&);

    constexpr Next() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        if (invoke_ == nullptr) {
            throw std::logic_error("route continuation is empty");
        }
        return invoke_(target_, context);
    }

private:
    friend class detail::RouteTable;

    constexpr Next(void* target, Invoke invoke) noexcept : target_(target), invoke_(invoke) {}

    void* target_{nullptr};
    Invoke invoke_{nullptr};
};

class Router final {
public:
    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) = delete;
    Router& operator=(Router&&) = delete;

private:
    template <typename ControllerT>
    friend class Controller;
    friend class detail::HttpServer;
    friend class detail::RouterImpl;

    std::unique_ptr<detail::RouterImpl> impl_;
};

}  // namespace ruvia
