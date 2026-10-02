#pragma once
#include <memory_resource>
#include <string>
namespace ruvia::detail {
// Owns a transport input block. QUIC includes the Quarter Stream ID; stream
// bytes are incremental Capsule Protocol input. Empty optional denotes EOF.
struct HttpDatagramInput final {
    std::pmr::string bytes;
    bool quic{};
};
}  // namespace ruvia::detail
