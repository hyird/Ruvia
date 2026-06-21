#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename Session>
class Http2RequestBodyReader final {
public:
    Http2RequestBodyReader(Session& session, std::uint32_t streamId) noexcept
        : session_(session), streamId_(streamId) {}

    [[nodiscard]] static Task<std::optional<std::string_view>> readThunk(void* target) {
        auto* self = static_cast<Http2RequestBodyReader*>(target);
        return self->session_.readBodyChunk(self->streamId_);
    }

private:
    Session& session_;
    std::uint32_t streamId_{0};
};

}  // namespace ruvia::detail
