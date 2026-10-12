#pragma once

#include <memory>
#include <memory_resource>

namespace ruvia {

namespace detail {

class router_impl;

// The registration surface a controller's RUVIA_ROUTES_BEGIN(type) block declares its
// routes into. One exists only while compile_route_plan() builds the process
// route plan, so an application never names this type: it appears only inside
// the macro-generated register_routes() signature. It is internal for that
// reason, and has no public members of its own.
class router final {
public:
    // `plan_resource` owns the compiled plan this router builds.
    explicit router(std::pmr::memory_resource* plan_resource);
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

private:
    struct impl_deleter_type {
        void operator()(router_impl* impl) const noexcept;
    };

    friend class router_impl;

    std::unique_ptr<router_impl, impl_deleter_type> impl_;
};

}  // namespace detail

}  // namespace ruvia
