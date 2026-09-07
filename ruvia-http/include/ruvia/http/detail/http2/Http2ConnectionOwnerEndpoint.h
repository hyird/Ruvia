#pragma once

#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

// Events retain this endpoint intrusively, so its address cannot be recycled
// while a stale lease/credit still exists. Detaching the target makes
// destruction safe when an event outlives its connection. The endpoint also
// retains the connection storage after owner destruction because request and
// body views can still refer to it.
class Http2ConnectionOwnerEndpoint final {
public:
    using AbandonRequest = void (*)(void*, std::uint32_t) noexcept;
    using AbandonCredit = void (*)(void*, std::uint32_t, std::uint32_t) noexcept;
    using DestroyStorage = void (*)(void*) noexcept;

    Http2ConnectionOwnerEndpoint(
        void* target, AbandonRequest abandonRequest, AbandonCredit abandonCredit) noexcept;

    void retain() noexcept;
    void retainStorage(void* storage, DestroyStorage destroyStorage) noexcept;
    void release() noexcept;
    void detach() noexcept;
    void abandonRequest(std::uint32_t streamId) noexcept;
    void abandonCredit(std::uint32_t streamId, std::uint32_t bytes) noexcept;

private:
    std::size_t references_{1};
    void* target_;
    AbandonRequest abandonRequest_;
    AbandonCredit abandonCredit_;
    void* retainedStorage_{nullptr};
    DestroyStorage destroyStorage_{nullptr};
};

}  // namespace ruvia::detail
