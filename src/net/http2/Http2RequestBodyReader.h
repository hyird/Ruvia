#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "../body/HttpRequestBodyFacade.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename Session>
class Http2RequestBodyReader final {
public:
    Http2RequestBodyReader(Session& session, std::uint32_t streamId) noexcept
        : session_(session), streamId_(streamId) {}

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        return session_.readBodyChunk(streamId_);
    }

private:
    Session& session_;
    std::uint32_t streamId_{0};
};

}  // namespace ruvia::detail
