#include "ruvia/http/detail/http2/Http2ConnectionOwnerEndpoint.h"

#include <exception>

namespace ruvia::detail {

Http2ConnectionOwnerEndpoint::Http2ConnectionOwnerEndpoint(
    void* target, AbandonRequest abandonRequest, AbandonCredit abandonCredit) noexcept
    : target_(target),
      abandonRequest_(abandonRequest),
      abandonCredit_(abandonCredit) {}

void Http2ConnectionOwnerEndpoint::retain() noexcept {
    ++references_;
}

void Http2ConnectionOwnerEndpoint::retainStorage(
    void* storage, DestroyStorage destroyStorage) noexcept {
    if (retainedStorage_ != nullptr || storage == nullptr || destroyStorage == nullptr) {
        std::terminate();
    }
    retainedStorage_ = storage;
    destroyStorage_ = destroyStorage;
}

void Http2ConnectionOwnerEndpoint::release() noexcept {
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        if (retainedStorage_ != nullptr) {
            destroyStorage_(retainedStorage_);
        }
        delete this;
    }
}

void Http2ConnectionOwnerEndpoint::detach() noexcept {
    target_ = nullptr;
}

void Http2ConnectionOwnerEndpoint::abandonRequest(std::uint32_t streamId) noexcept {
    if (target_ != nullptr) {
        abandonRequest_(target_, streamId);
    }
}

void Http2ConnectionOwnerEndpoint::abandonCredit(
    std::uint32_t streamId, std::uint32_t bytes) noexcept {
    if (target_ != nullptr) {
        abandonCredit_(target_, streamId, bytes);
    }
}

}  // namespace ruvia::detail
