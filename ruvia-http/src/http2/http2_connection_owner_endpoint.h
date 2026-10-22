#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace ruvia::detail {

class http2_connection;

// Events retain this endpoint intrusively, so its address cannot be recycled
// while a stale lease/credit still exists. Detaching the connection makes
// destruction safe when an event outlives its owner. The endpoint also retains
// the connection storage after owner destruction because request and body views
// can still refer to it.
class http2_connection_owner_endpoint final {
public:
    using destroy_storage_type = void (*)(void*) noexcept;

    http2_connection_owner_endpoint(http2_connection* connection, std::pmr::memory_resource* resource) noexcept;

    void retain() noexcept;
    void retain_storage(void* storage, destroy_storage_type destroy_storage) noexcept;
    void release() noexcept;
    void detach() noexcept;
    [[nodiscard]] http2_connection* connection() const noexcept {
        return connection_;
    }
    void abandon_request(std::uint32_t stream_id) noexcept;
    void abandon_credit(std::uint32_t stream_id, std::uint32_t bytes) noexcept;

private:
    std::size_t references_{1};
    http2_connection* connection_;
    void* retained_storage_{nullptr};
    destroy_storage_type destroy_storage_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia::detail
