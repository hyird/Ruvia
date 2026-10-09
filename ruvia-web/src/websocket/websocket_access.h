#pragma once

#include <memory_resource>

#include "ruvia/web/websocket.h"

namespace ruvia::detail {

struct websocket_access final {
    static void noop_abort(void*) noexcept {}

    // Unbound facade construction is retained for isolated capability tests;
    // runtime-created facades always borrow their connection's worker handle.
    [[nodiscard]] static websocket make(std::pmr::memory_resource& resource, void* target,
        websocket::read_type read, websocket::write_type write, websocket::close_type close) noexcept {
        return websocket(resource, nullptr, target, read, write, close, &noop_abort);
    }

    [[nodiscard]] static websocket make(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, websocket::read_type read, websocket::write_type write, websocket::close_type close) noexcept {
        return websocket(resource, &worker_value, target, read, write, close, &noop_abort);
    }

    [[nodiscard]] static websocket make(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, websocket::read_type read, websocket::write_type write, websocket::close_type close,
        websocket::abort_type abort) noexcept {
        return websocket(resource, &worker_value, target, read, write, close, abort);
    }
};

}  // namespace ruvia::detail
