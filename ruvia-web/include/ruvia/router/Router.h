#pragma once

#include <memory>

namespace ruvia {

namespace detail {

class RouterImpl;
struct RouterImplDeleter final {
    void operator()(RouterImpl* impl) const noexcept;
};

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
    template <typename ControllerT>
    friend class Controller;
    friend class detail::RouterImpl;

    std::unique_ptr<detail::RouterImpl, detail::RouterImplDeleter> impl_;
};

}  // namespace ruvia
