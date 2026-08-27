#pragma once

#include "ruvia/web/detail/body/HttpStreamBodyReader.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

template <typename Stream>
class LazyBufferedBody final {
public:
    LazyBufferedBody(Stream& stream, std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource, std::string_view initialBodyAndPipeline,
        Http1RequestBodyPlan bodyPlan, ProtocolByteLimit bodyLimit,
        ConnectionScanner::Entry& scannerEntry)
        : reader_(
              stream, workerAllocator, initialBodyAndPipeline, bodyPlan, bodyLimit, scannerEntry),
          body_(pmrResourceOrDefault(requestResource)) {}

    [[nodiscard]] Http1RequestBodyConsumption consumption() const noexcept {
        return reader_.consumption();
    }

    void takePipeline(std::pmr::string& stash) {
        reader_.takePipeline(stash);
    }

    [[nodiscard]] Task<std::string_view> readAll() {
        co_return co_await reader_.readAll(body_);
    }

    Task<void> discard() {
        while (co_await reader_.read()) {
        }
    }

private:
    StreamBodyReader<Stream> reader_;
    std::pmr::string body_;
};

}  // namespace ruvia::detail
