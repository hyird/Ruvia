#pragma once

#include <memory_resource>

#include "ruvia/web/websocket.h"

namespace ruvia::detail {

struct websocket_access final {
    [[nodiscard]] static websocket make(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, websocket::read_type read, websocket::write_type write, websocket::close_type close,
        websocket::abort_type abort) noexcept {
        return websocket(resource, worker_value, target, read, write, close, abort);
    }
};

}  // namespace ruvia::detail
