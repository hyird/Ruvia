#include "ruvia/web/detail/server/HttpConnectionState.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>

#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::detail {
namespace {

// The read buffer grows up to kMaxHttpHeaderBytes (see growReadBuffer); a
// capacity past that means it spilled to hold a body/burst, so reclaim it back
// to the initial size on return. Expressed in terms of the header limit so the
// two move together if that limit is ever retuned.
constexpr std::size_t kReadBufferShrinkCapacityBytes = kMaxHttpHeaderBytes;
// Upper bound on work sets retained per worker. Beyond this, returned work sets
// release upstream so the free list never grows unbounded after a burst.
constexpr std::size_t kMaxPooledWorkSets = 64;

}  // namespace

// The read buffer is sized to the initial capacity up front so an acquired work
// set is immediately usable; resetForReuse() trims it back on return.
ConnectionWorkSet::ConnectionWorkSet(WorkerMemory& memory)
    : readBuffer(memory.allocator<char>()),
      responseHead(memory.allocator<char>()),
      fileChunk(memory.allocator<char>()) {
    resizePmrStringForOverwrite(readBuffer, kInitialReadBufferBytes);
}

void ConnectionWorkSet::resetForReuse() {
    // The read buffer never grows past the 64KB header limit, so this only ever
    // resizes (never rebuilds) and cannot throw in practice; the pool's release
    // still guards against it.
    trimReadBufferStorage(readBuffer, 0);
    responseHead.reset();
    // parsed is fully overwritten by the next parseHead(); fileChunk/parser
    // carry no cross-request state worth clearing.
}

ConnectionWorkSetPool::ConnectionWorkSetPool(WorkerMemory& memory) noexcept
    : memory_(&memory) {}

ConnectionWorkSetPool::~ConnectionWorkSetPool() {
    auto* current = freeHead_;
    while (current != nullptr) {
        auto* next = current->poolNext;
        destroyPmrObject(current, memory_->resource());
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
    return constructPmrObject<ConnectionWorkSet>(memory_->resource(), *memory_);
}

void ConnectionWorkSetPool::release(ConnectionWorkSet* workSet) noexcept {
    if (workSet == nullptr) {
        return;
    }
    const auto destroy = [this](ConnectionWorkSet* victim) noexcept {
        destroyPmrObject(victim, memory_->resource());
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

void installConnectionReadBufferPipeline(
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::string_view pipeline) {
    // `pipeline` is request-scoped storage handed over by a body runtime, never
    // an alias of readBuffer, so this copies rather than shifts in place.
    if (pipeline.size() > readBuffer.size()) {
        resizePmrStringForOverwrite(readBuffer, pipeline.size());
    }
    if (!pipeline.empty()) {
        std::memcpy(readBuffer.data(), pipeline.data(), pipeline.size());
    }
    usedBytes = pipeline.size();
}

void applyReusableHttp1RequestBufferCompletion(
    const Http1RequestBufferCompletion& completion,
    std::pmr::string& readBuffer,
    std::size_t& usedBytes) {
    if (const auto* compaction = completion.compaction()) {
        if (compaction->consumedBytes() > usedBytes) {
            std::terminate();
        }
        compactConnectionReadBuffer(
            readBuffer,
            usedBytes,
            compaction->consumedBytes());
        return;
    }
    if (const auto* restore = completion.pipelineRestore()) {
        installConnectionReadBufferPipeline(
            readBuffer,
            usedBytes,
            restore->pipeline());
        return;
    }
    // A discarded buffer is valid only when the connection plan closes. The
    // session must never reach reusable cleanup with that alternative.
    std::terminate();
}

void trimReadBufferStorage(std::pmr::string& readBuffer, std::size_t usedBytes) {
    if (usedBytes > kInitialReadBufferBytes) {
        return;
    }

    if (readBuffer.capacity() > kReadBufferShrinkCapacityBytes) {
        std::pmr::string compact(readBuffer.get_allocator());
        resizePmrStringForOverwrite(compact, kInitialReadBufferBytes);
        if (usedBytes > 0) {
            std::memcpy(compact.data(), readBuffer.data(), usedBytes);
        }
        readBuffer = std::move(compact);
        return;
    }

    if (readBuffer.size() != kInitialReadBufferBytes) {
        resizePmrStringForOverwrite(readBuffer, kInitialReadBufferBytes);
    }
}

void growReadBuffer(std::pmr::string& readBuffer, std::size_t usedBytes) {
    if (usedBytes == readBuffer.size() && readBuffer.size() < kMaxHttpHeaderBytes) {
        resizePmrStringForOverwrite(readBuffer, std::min(readBuffer.size() * 2, kMaxHttpHeaderBytes));
    }
}

}  // namespace ruvia::detail
