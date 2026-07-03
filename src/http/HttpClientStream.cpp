#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <span>
#include <utility>

#include "ruvia/http/HttpClient.h"
#include "client/FetchStreamSource.h"

namespace ruvia {

namespace detail {

void FetchStreamSourceDeleter::operator()(FetchStreamSource* source) const noexcept {
    if (source != nullptr) {
        source->destroy();
    }
}

}  // namespace detail

FetchResponseStream::FetchResponseStream(
    std::unique_ptr<detail::FetchStreamSource, detail::FetchStreamSourceDeleter> source) noexcept
    : source_(std::move(source)) {}

FetchResponseStream::FetchResponseStream(FetchResponseStream&&) noexcept = default;
FetchResponseStream& FetchResponseStream::operator=(FetchResponseStream&&) noexcept = default;
FetchResponseStream::~FetchResponseStream() = default;

int FetchResponseStream::statusCode() const noexcept {
    return source_ ? source_->statusCode() : 0;
}

std::span<const FetchResponseHeader> FetchResponseStream::headers() const noexcept {
    if (!source_) {
        return {};
    }
    const auto& headerList = source_->headers();
    return std::span<const FetchResponseHeader>(headerList.data(), headerList.size());
}

Task<std::pmr::string> FetchResponseStream::readChunk() {
    if (!source_) {
        co_return std::pmr::string{};
    }
    co_return co_await source_->readChunk();
}

void FetchResponseStream::close() noexcept {
    if (source_) {
        source_->close();
        source_.reset();
    }
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_HTTP_CLIENT
