#pragma once

#include "ruvia/http/HttpResponseFile.h"
#include "ruvia/http/detail/util/NativePath.h"

namespace ruvia::detail {

using ResponseFileIdentity = ruvia::HttpResponseFileIdentity;
using ResponseFileBody = ruvia::HttpResponseFileView;

// Internal compatibility helper for protocol/runtime code constructing a
// descriptor from a native path.
struct ResponseFileBodyAccess final {
    [[nodiscard]] static constexpr ResponseFileBody make(const HttpNativePathChar* path,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        ResponseFileIdentity identity) noexcept {
        return ResponseFileBody(path, size, offset, length, identity);
    }
};

}  // namespace ruvia::detail
