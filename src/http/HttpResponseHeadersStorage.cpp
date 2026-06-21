#include "ruvia/http/HttpResponse.h"

#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <utility>

namespace ruvia {

HttpResponseHeaders::HttpResponseHeaders(std::pmr::memory_resource* resource)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      heap_(resource_) {}

HttpResponseHeaders::~HttpResponseHeaders() {
    clear();
}

HttpResponseHeaders::HttpResponseHeaders(HttpResponseHeaders&& other) noexcept
    : resource_(other.resource_),
      heap_(resource_) {
    moveFrom(std::move(other));
}

HttpResponseHeaders& HttpResponseHeaders::operator=(HttpResponseHeaders&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    resource_ = other.resource_;
    heap_ = std::pmr::vector<HttpResponseHeader>(resource_);
    spilled_ = false;
    moveFrom(std::move(other));
    return *this;
}

void HttpResponseHeaders::reserve(std::size_t count) {
    if (count <= kInlineCapacity) {
        return;
    }

    spill();
    heap_.reserve(count);
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

void HttpResponseHeaders::spill() {
    if (spilled_) {
        return;
    }

    heap_.reserve(std::max<std::size_t>(kInlineCapacity * 2, size_ + 1));
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
