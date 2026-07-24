#pragma once

#include "ruvia/http/HttpResponse.h"

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

struct HttpResponseHeadersAccess final {
    using iterator = HttpResponseHeader*;

    [[nodiscard]] static iterator begin(HttpResponseHeaders& headers) noexcept {
        return headers.begin();
    }

    [[nodiscard]] static iterator end(HttpResponseHeaders& headers) noexcept {
        return headers.end();
    }

    static void release(HttpResponseHeaders& headers, HttpResponseHeader& header) noexcept {
        headers.releaseHeader(header);
    }

    static void truncate(HttpResponseHeaders& headers, iterator begin, iterator write) {
        if (headers.spilled_) {
            headers.heap_.erase(headers.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin), headers.heap_.end());
            return;
        }
        headers.size_ = static_cast<std::size_t>(write - begin);
    }

    [[nodiscard]] static HttpResponseHeader& add(HttpResponseHeaders& headers, std::string_view name, std::string_view value, std::uint32_t knownBit) {
        return headers.add(name, value, knownBit);
    }

    static void assign(HttpResponseHeaders& headers, HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit) {
        headers.assign(header, name, value, knownBit);
    }

    [[nodiscard]] static HttpResponseHeader& addUninitializedValue(HttpResponseHeaders& headers, std::string_view name, std::size_t valueSize, std::uint32_t knownBit) {
        return headers.addUninitializedValue(name, valueSize, knownBit);
    }

    [[nodiscard]] static HttpResponseHeader& addStableView(HttpResponseHeaders& headers, std::string_view name, std::string_view value, std::uint32_t knownBit) {
        return headers.addStableView(name, value, knownBit);
    }

    static void assignStableView(HttpResponseHeaders& headers, HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit) {
        headers.assignStableView(header, name, value, knownBit);
    }
};

}  // namespace ruvia::detail
