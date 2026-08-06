#pragma once

#include <memory>

namespace ruvia {

template <typename ControllerT>
class Controller;

namespace detail {

class RouterImpl;

// The route table a controller's RUVIA_ROUTES_BEGIN block registers into.
// Controllers register themselves at static initialization and App owns the one
// router that results, so an application never names this type: it appears only
// inside the macro-generated registerRoutes() signature. It is internal for that
// reason, and has no public members of its own.
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
        void operator()(RouterImpl* impl) const noexcept;
    };

    template <typename ControllerT>
    friend class ::ruvia::Controller;
    friend class RouterImpl;

    std::unique_ptr<RouterImpl, ImplDeleter> impl_;
};

}  // namespace detail

}  // namespace ruvia
