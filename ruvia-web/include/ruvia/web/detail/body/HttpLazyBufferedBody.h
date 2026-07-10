#pragma once

#include "ruvia/web/detail/body/HttpStreamBodyReader.h"
#include "ruvia/memory/PmrResource.h"

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
          body_(pmrResourceOrDefault(requestResource)),
          hasBody_(contentLength > 0 || chunked || !transferCodings.empty()) {}

    [[nodiscard]] bool consumed() const noexcept {
        return !hasBody_ || reader_.finished();
    }

    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
        reader_.restorePipeline(readBuffer, usedBytes);
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
