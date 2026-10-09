#pragma once

#include <memory>

namespace ruvia {

template <typename controller_t_type>
class controller;

namespace detail {

class router_impl;

// The route table a controller's RUVIA_ROUTES_BEGIN block registers into.
// Controllers register themselves at static initialization and application owns the one
// router that results, so an application never names this type: it appears only
// inside the macro-generated register_routes() signature. It is internal for that
// reason, and has no public members of its own.
class router final {
public:
    router();
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

private:
    struct impl_deleter_type {
        void operator()(router_impl* impl) const noexcept;
    };

    template <typename controller_t_type>
    friend class ::ruvia::controller;
    friend class router_impl;

    std::unique_ptr<router_impl, impl_deleter_type> impl_;
};

}  // namespace detail

}  // namespace ruvia
