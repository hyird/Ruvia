#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

// Representation-independent field-section accounting. The fixed 32-byte
// charge matches HTTP/2's decoded header-list definition and also reserves a
// conservative metadata budget for protocols whose wire syntax is smaller.
// Pseudo-fields participate in the byte budget but are counted separately from
// kMaxHttpHeaderFields by the protocol state that owns them.
class HttpHeaderSectionSize final {
public:
    [[nodiscard]] bool add(
        std::string_view name,
        std::string_view value) noexcept {
        constexpr std::size_t kFieldMetadataBytes = 32;
        if (name.size() > kMaxHttpHeaderBytes ||
            value.size() > kMaxHttpHeaderBytes - name.size()) {
            return false;
        }
        auto fieldBytes = name.size() + value.size();
        if (fieldBytes > kMaxHttpHeaderBytes - kFieldMetadataBytes) {
            return false;
        }
        fieldBytes += kFieldMetadataBytes;
        if (fieldBytes > kMaxHttpHeaderBytes - bytes_) {
            return false;
        }
        bytes_ += fieldBytes;
        return true;
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
