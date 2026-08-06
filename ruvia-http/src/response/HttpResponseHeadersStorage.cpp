#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory_resource>
#include <utility>

namespace ruvia {

HttpResponseHeaders::HttpResponseHeaders(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : resource_(resource),
      heap_(resource_) {}

HttpResponseHeaders::~HttpResponseHeaders() {
    clear();
}

HttpResponseHeaders::HttpResponseHeaders(HttpResponseHeaders&& other) noexcept
    : resource_(other.resource_),
      heap_(resource_) {
    moveFrom(std::move(other));
}

void HttpResponseHeaders::reserve(std::size_t count) {
    if (count <= kInlineCapacity) {
        return;
    }

    if (!spilled_) {
        spill(count);
        return;
    }
    heap_.reserve(count);
}

HttpResponseHeader& HttpResponseHeaders::appendPreparedHeader(HttpResponseHeader header) noexcept {
    if (!spilled_) {
        if (size_ == kInlineCapacity) {
            std::terminate();
        }
        auto* const target = inlineData() + size_;
        *target = header;
        ++size_;
        return *target;
    }

    if (heap_.size() == heap_.capacity()) {
        std::terminate();
    }
    heap_.push_back(header);
    return heap_.back();
}

HttpResponseHeader* HttpResponseHeaders::inlineData() noexcept {
    return reinterpret_cast<HttpResponseHeader*>(inline_.data());
}

const HttpResponseHeader* HttpResponseHeaders::inlineData() const noexcept {
    return reinterpret_cast<const HttpResponseHeader*>(inline_.data());
}

HttpResponseHeader* HttpResponseHeaders::data() noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

const HttpResponseHeader* HttpResponseHeaders::data() const noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

void HttpResponseHeaders::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        releaseHeader(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void HttpResponseHeaders::spill(std::size_t minCapacity) {
    if (spilled_) {
        return;
    }

    // Reserve is the only allocation step. Populate the new table only after
    // it succeeds, and publish `spilled_`/`size_` last. The descriptors are
    // trivially copyable and the vector has enough capacity, so a hypothetical
    // exception during the copy can leave only non-owning duplicate descriptors
    // in the still-inactive heap table; the next retry clears them before
    // publishing anything. The inline table remains the sole owner until then.
    heap_.clear();
    heap_.reserve(std::max<std::size_t>(kInlineCapacity * 2, minCapacity));
    auto* items = inlineData();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void HttpResponseHeaders::moveFrom(HttpResponseHeaders&& other) noexcept {
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(InlineStorage));
    }
    size_ = other.size_;
    other.size_ = 0;
}

}  // namespace ruvia
