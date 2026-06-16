#include "HttpConnectionState.h"

#include <algorithm>
#include <cstring>

#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kInitialReadBufferBytes = 8 * 1024;
constexpr std::size_t kReadBufferShrinkCapacityBytes = 64 * 1024;

}  // namespace

ConnectionState::ConnectionState(WorkerMemory& memory)
    : readBuffer(memory.allocator<char>()),
      responseHead(memory.allocator<char>()),
      fileChunk(memory.allocator<char>()) {
    readBuffer.resize(kInitialReadBufferBytes);
}

void compactConnectionReadBuffer(
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t consumedBytes) noexcept {
    const auto remainingBytes = usedBytes - consumedBytes;
    if (remainingBytes > 0) {
        std::memmove(readBuffer.data(), readBuffer.data() + consumedBytes, remainingBytes);
    }
    usedBytes = remainingBytes;
}

void trimReadBufferStorage(std::pmr::string& readBuffer, std::size_t usedBytes) {
    if (usedBytes > kInitialReadBufferBytes) {
        return;
    }

    if (readBuffer.capacity() > kReadBufferShrinkCapacityBytes) {
        std::pmr::string compact(readBuffer.get_allocator());
        compact.resize(kInitialReadBufferBytes);
        if (usedBytes > 0) {
            std::memcpy(compact.data(), readBuffer.data(), usedBytes);
        }
        readBuffer = std::move(compact);
        return;
    }

    if (readBuffer.size() < kInitialReadBufferBytes) {
        readBuffer.resize(kInitialReadBufferBytes);
        return;
    }

    if (readBuffer.size() > kInitialReadBufferBytes) {
        readBuffer.resize(kInitialReadBufferBytes);
    }
}

void growReadBuffer(std::pmr::string& readBuffer, std::size_t usedBytes, const HttpParseResult& parsed) {
    if (parsed.consumedBytes > readBuffer.size()) {
        readBuffer.resize(parsed.consumedBytes);
        return;
    }

    if (usedBytes == readBuffer.size() && readBuffer.size() < kMaxHttpHeaderBytes) {
        readBuffer.resize(std::min(readBuffer.size() * 2, kMaxHttpHeaderBytes));
    }
}

}  // namespace ruvia::detail
