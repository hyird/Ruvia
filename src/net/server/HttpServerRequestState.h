#pragma once

#include "../../http/HttpParserInternal.h"
#include "ruvia/memory/MemoryPool.h"

#include <cstddef>
#include <optional>
#include <span>

namespace ruvia::detail {

inline constexpr std::size_t kRequestArenaStackBytes = 4 * 1024;

inline bool contentLengthExceedsLimit(std::size_t contentLength, std::size_t limit) noexcept {
    return limit != 0 && contentLength > limit;
}

inline bool shouldKeepAlive(const HttpServerParseResult& parsed) noexcept {
    if (parsed.flags.connectionClose) {
        return false;
    }
    if (parsed.flags.connectionKeepAlive) {
        return true;
    }
    return parsed.request.httpVersion() == "HTTP/1.1";
}

inline bool wantsContinue(const HttpServerParseResult& parsed) noexcept {
    return parsed.flags.expectContinue;
}

inline RequestMemory& emplaceRequestMemory(
    std::optional<RequestMemory>& storage,
    WorkerMemory& memory,
    std::span<std::byte> initialBuffer) {
    const auto initialBytes = memory.requestInitialBufferBytes();
    if (initialBytes <= initialBuffer.size()) {
        storage.emplace(memory, initialBuffer.first(initialBytes));
    } else {
        storage.emplace(memory);
    }
    return *storage;
}

}  // namespace ruvia::detail
