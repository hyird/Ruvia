#pragma once

#include "ruvia/web/detail/body/HttpStreamBodyReader.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

template <typename Stream>
class LazyBufferedBody final {
public:
    LazyBufferedBody(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view initialBodyAndPipeline,
        Http1RequestBodyPlan bodyPlan,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry)
        : reader_(
              stream,
              workerAllocator,
              initialBodyAndPipeline,
              bodyPlan,
              maxBodyBytes,
              scannerEntry),
          body_(pmrResourceOrDefault(requestResource)) {}

    [[nodiscard]] Http1RequestBodyConsumption consumption() const noexcept {
        return reader_.consumption();
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
    bool read_{false};
};

}  // namespace ruvia::detail
