#pragma once

#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/http/HttpContentCodec.h"

namespace ruvia::detail {

struct HttpContentDecodeResultAccess final {
    [[nodiscard]] static HttpContentDecodeResult decoded(std::pmr::string bytes) noexcept {
        return HttpContentDecodeResult(HttpDecodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentDecodeResult failure(HttpContentDecodeError error) noexcept {
        return HttpContentDecodeResult(HttpContentDecodeFailure(error));
    }
};

struct HttpContentEncodeResultAccess final {
    [[nodiscard]] static HttpContentEncodeResult encoded(std::pmr::string bytes) noexcept {
        return HttpContentEncodeResult(HttpEncodedContent(std::move(bytes)));
    }

    [[nodiscard]] static HttpContentEncodeResult failure(HttpContentEncodeError error) noexcept {
        return HttpContentEncodeResult(HttpContentEncodeFailure(error));
    }
};

}  // namespace ruvia::detail
