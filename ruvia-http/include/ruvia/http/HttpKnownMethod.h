#pragma once

#include <string_view>

namespace ruvia {

enum class HttpKnownMethod {
    kGet,
    kPost,
    kPut,
    kDelete,
    kPatch,
    kHead,
    kOptions,
    kConnect,
    kUnknown
};

// HTTP methods are an extensible, case-sensitive token space. HttpKnownMethod is
// only the framework's fixed semantic classification; it is never the wire value.
[[nodiscard]] HttpKnownMethod classifyHttpMethod(std::string_view method) noexcept;
[[nodiscard]] std::string_view knownHttpMethodToken(HttpKnownMethod method) noexcept;
[[nodiscard]] bool isValidHttpMethodToken(std::string_view method) noexcept;

}  // namespace ruvia
