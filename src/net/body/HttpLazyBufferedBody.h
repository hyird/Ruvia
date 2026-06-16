#pragma once

#include "HttpStreamBodyReader.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
class LazyBufferedBody final {
public:
    LazyBufferedBody(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view initialBodyAndPipeline,
        std::size_t contentLength,
        bool chunked,
        HttpTransferCodings transferCodings,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry,
        bool sendContinue)
        : reader_(
              stream,
              workerAllocator,
              initialBodyAndPipeline,
              contentLength,
              chunked,
              transferCodings,
              maxBodyBytes,
              scannerEntry,
              sendContinue),
          body_(requestResource == nullptr ? ProcessMemory::instance().upstreamResource() : requestResource),
          hasBody_(contentLength > 0 || chunked || !transferCodings.empty()) {}

    [[nodiscard]] bool consumed() const noexcept {
        return !hasBody_ || reader_.finished();
    }

    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
        reader_.restorePipeline(readBuffer, usedBytes);
    }

    [[nodiscard]] static Task<std::string_view> readAllThunk(void* target) {
        return static_cast<LazyBufferedBody*>(target)->readAll();
    }

    static Task<void> discardThunk(void* target) {
        return static_cast<LazyBufferedBody*>(target)->discard();
    }

    [[nodiscard]] Task<std::string_view> readAll() {
        if (read_) {
            co_return bodyView_;
        }
        bodyView_ = co_await reader_.readAll(body_);
        read_ = true;
        co_return bodyView_;
    }

    Task<void> discard() {
        if (read_) {
            co_return;
        }
        while (co_await reader_.read()) {}
    }

private:
    StreamBodyReader<Stream> reader_;
    std::pmr::string body_;
    std::string_view bodyView_;
    bool hasBody_{false};
    bool read_{false};
};

}  // namespace ruvia::detail
