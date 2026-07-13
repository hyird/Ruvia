#pragma once

#include <memory>

namespace ruvia {

namespace detail {

class RouterImpl;

}  // namespace detail

template <typename ControllerT>
class Controller;

class Router final {
public:
    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) = delete;
    Router& operator=(Router&&) = delete;

private:
    struct ImplDeleter final {
        void operator()(detail::RouterImpl* impl) const noexcept;
    };

    template <typename ControllerT>
    friend class Controller;
    friend class detail::RouterImpl;

    std::unique_ptr<detail::RouterImpl, ImplDeleter> impl_;
};

}  // namespace ruvia
