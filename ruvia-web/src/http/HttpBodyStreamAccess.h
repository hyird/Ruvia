#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpBodyStream.h"

#include <string_view>

namespace ruvia::detail {

struct HttpBodyStreamAccess final {
    using NextChunk = Task<std::string_view> (*)(void*);

    [[nodiscard]] static Task<std::string_view> nextChunk(const HttpBodyStream& stream) {
        if (stream.next_ == nullptr) {
            co_return std::string_view{};
        }
        const auto next = reinterpret_cast<NextChunk>(stream.next_);
        co_return co_await next(stream.target_);
    }
};

}  // namespace ruvia::detail
