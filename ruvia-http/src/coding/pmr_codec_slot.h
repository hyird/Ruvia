#pragma once

#include <memory_resource>
#include <utility>

#include "ruvia/http/detail/util/http_pmr_object.h"

// A reusable codec slot: native compression libraries keep pointers into
// their stream, so the codec lives in one stable allocation from the owner's
// resource. `codec_type` provides initialized() and reset().

namespace ruvia::detail {

// The slot's codec, created on first use. A codec that failed to initialize is
// not kept, so the next use retries; nullptr reports that failure.
template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_pmr_codec(
    codec_type*& slot, std::pmr::memory_resource* resource, args_type&&... args) {
    if (slot == nullptr) {
        auto* created = construct_http_pmr_object<codec_type>(
            http_resolved_pmr_resource_tag{}, resource, std::forward<args_type>(args)...);
        if (!created->initialized()) {
            destroy_http_pmr_object(http_resolved_pmr_resource_tag{}, created, resource);
            return nullptr;
        }
        slot = created;
    }
    return slot;
}

// Returns an acquired slot's codec to its initial state on every exit,
// exceptions included. A codec that cannot reset is dropped and recreated by
// the next use.
template <typename codec_type>
class pmr_codec_reset_guard final {
public:
    pmr_codec_reset_guard(codec_type*& slot, std::pmr::memory_resource* resource) noexcept
        : slot_(slot),
          resource_(resource) {}
    pmr_codec_reset_guard(const pmr_codec_reset_guard&) = delete;
    pmr_codec_reset_guard& operator=(const pmr_codec_reset_guard&) = delete;

    ~pmr_codec_reset_guard() {
        if (!slot_->reset()) {
            destroy_http_pmr_object(http_resolved_pmr_resource_tag{}, std::exchange(slot_, nullptr), resource_);
        }
    }

private:
    codec_type*& slot_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia::detail
