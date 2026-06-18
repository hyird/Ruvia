#include "HttpConnectionState.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kInitialReadBufferBytes = 8 * 1024;
constexpr std::size_t kReadBufferShrinkCapacityBytes = 64 * 1024;
// Upper bound on work sets retained per worker. Beyond this, returned work sets
// free to the upstream (mimalloc) so the free list never grows unbounded under
// a burst of idle transitions.
constexpr std::size_t kMaxPooledWorkSets = 64;

}  // namespace

// The read buffer is sized to the initial capacity up front so an acquired work
// set is immediately usable; resetForReuse() trims it back on return.
ConnectionWorkSet::ConnectionWorkSet(WorkerMemory& memory)
    : readBuffer(memory.allocator<char>()),
      responseHead(memory.allocator<char>()),
      fileChunk(memory.allocator<char>()) {
    readBuffer.resize(kInitialReadBufferBytes);
}

void ConnectionWorkSet::resetForReuse() {
    // The read buffer never grows past the 64KB header limit, so this only ever
    // resizes (never rebuilds) and cannot throw in practice; the pool's release
    // still guards against it.
    trimReadBufferStorage(readBuffer, 0);
    usedBytes = 0;
    responseHead.reset();
    // parsed is fully overwritten by the next parseHeaders(); fileChunk/parser
    // carry no cross-request state worth clearing.
}

ConnectionWorkSetPool::ConnectionWorkSetPool(WorkerMemory& memory) noexcept
    : memory_(&memory) {}

ConnectionWorkSetPool::~ConnectionWorkSetPool() {
    auto* current = freeHead_;
    while (current != nullptr) {
        auto* next = current->poolNext;
        std::destroy_at(current);
        memory_->resource()->deallocate(current, sizeof(ConnectionWorkSet), alignof(ConnectionWorkSet));
        current = next;
    }
}

ConnectionWorkSet* ConnectionWorkSetPool::acquire() {
    if (freeHead_ != nullptr) {
        auto* workSet = freeHead_;
        freeHead_ = workSet->poolNext;
        workSet->poolNext = nullptr;
        --freeCount_;
        return workSet;
    }
    void* storage = memory_->resource()->allocate(sizeof(ConnectionWorkSet), alignof(ConnectionWorkSet));
    return std::construct_at(static_cast<ConnectionWorkSet*>(storage), *memory_);
}

void ConnectionWorkSetPool::release(ConnectionWorkSet* workSet) noexcept {
    if (workSet == nullptr) {
        return;
    }
    const auto destroy = [this](ConnectionWorkSet* victim) noexcept {
        std::destroy_at(victim);
        memory_->resource()->deallocate(victim, sizeof(ConnectionWorkSet), alignof(ConnectionWorkSet));
    };
    if (freeCount_ >= kMaxPooledWorkSets) {
        destroy(workSet);
        return;
    }
    try {
        workSet->resetForReuse();
    } catch (...) {
        // resetForReuse can only throw if the read-buffer rebuild OOMs; drop the
        // work set rather than pooling it, keeping release noexcept.
        destroy(workSet);
        return;
    }
    workSet->poolNext = freeHead_;
    freeHead_ = workSet;
    ++freeCount_;
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
