#pragma once

#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/detail/context/Streaming.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ruvia {

namespace detail {

class MultipartBoundaryMarkers final {
public:
    MultipartBoundaryMarkers(std::string_view boundary, std::pmr::memory_resource* resource)
        : line_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          prefix_(line_.get_allocator().resource()) {
        line_.reserve(boundary.size() + 2);
        line_.append("--");
        line_.append(boundary.data(), boundary.size());
        prefix_.reserve(line_.size() + 2);
        prefix_.append("\r\n");
        prefix_.append(line_);
    }

    [[nodiscard]] std::string_view line() const noexcept {
        return line_;
    }

    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

private:
    std::pmr::string line_;
    std::pmr::string prefix_;
};

}  // namespace detail

struct MultipartStreamPart {
    std::string_view name;
    std::string_view filename;
    std::string_view contentType;
    std::string_view body;
    bool partBegin{false};
    bool partEnd{false};
};

class MultipartReader final {
public:
    MultipartReader(BodyReader& bodyReader, std::string_view boundary, std::pmr::memory_resource* resource)
        : bodyReader_(bodyReader),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          buffer_(resource_),
          boundary_(boundary, resource_),
          currentName_(resource_),
          currentFilename_(resource_),
          currentContentType_(resource_) {}

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> read() {
        for (;;) {
            compactPending();
            switch (state_) {
                case State::kBoundary:
                    co_await processBoundary();
                    if (state_ == State::kDone) {
                        co_return std::nullopt;
                    }
                    break;
                case State::kHeaders:
                    co_await processHeaders();
                    break;
                case State::kBody:
                    if (auto part = co_await readBodyChunk()) {
                        co_return part;
                    }
                    break;
                case State::kDone:
                    co_return std::nullopt;
            }
        }
    }

private:
    enum class State {
        kBoundary,
        kHeaders,
        kBody,
        kDone
    };

    static constexpr std::size_t kCompactConsumedPrefixBytes = 64 * 1024;

    [[nodiscard]] std::string_view bufferView() const noexcept {
        if (bufferOffset_ >= buffer_.size()) {
            return {};
        }
        return std::string_view(buffer_.data() + bufferOffset_, buffer_.size() - bufferOffset_);
    }

    void consume(std::size_t bytes) noexcept {
        const auto available = bufferView().size();
        bufferOffset_ += std::min(bytes, available);
        if (bufferOffset_ == buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
        }
    }

    void compactConsumedPrefix() {
        if (bufferOffset_ == 0) {
            return;
        }
        if (bufferOffset_ == buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
            return;
        }
        if (bufferOffset_ < kCompactConsumedPrefixBytes) {
            return;
        }
        const auto remaining = buffer_.size() - bufferOffset_;
        std::memmove(buffer_.data(), buffer_.data() + bufferOffset_, remaining);
        buffer_.resize(remaining);
        bufferOffset_ = 0;
    }

    void compactPending() {
        if (pendingEraseBytes_ == 0) {
            return;
        }
        consume(pendingEraseBytes_);
        pendingEraseBytes_ = 0;
    }

    Task<bool> appendMore() {
        compactConsumedPrefix();
        auto chunk = co_await bodyReader_.read();
        if (!chunk) {
            co_return false;
        }
        buffer_.append(chunk->data(), chunk->size());
        co_return true;
    }

    Task<void> processBoundary() {
        for (;;) {
            if (bufferView().starts_with("\r\n")) {
                consume(2);
            }
            while (bufferView().size() < boundary_.line().size() + 2) {
                if (!(co_await appendMore())) {
                    throw std::invalid_argument("invalid multipart body");
                }
            }
            const auto buffer = bufferView();
            if (!buffer.starts_with(boundary_.line())) {
                throw std::invalid_argument("invalid multipart body");
            }
            const auto afterBoundary = boundary_.line().size();
            if (buffer.substr(afterBoundary, 2) == "--") {
                state_ = State::kDone;
                co_return;
            }
            if (buffer.substr(afterBoundary, 2) == "\r\n") {
                consume(afterBoundary + 2);
                state_ = State::kHeaders;
                co_return;
            }
            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
        }
    }

    Task<void> processHeaders() {
        for (;;) {
            const auto buffer = bufferView();
            const auto headersEnd = buffer.find("\r\n\r\n");
            if (headersEnd == std::string_view::npos) {
                if (!(co_await appendMore())) {
                    throw std::invalid_argument("invalid multipart body");
                }
                continue;
            }

            const auto headers = buffer.substr(0, headersEnd);
            detail::HttpMultipartPartHeaders partHeaders;
            switch (detail::httpParseMultipartPartHeaders(headers, partHeaders)) {
                case detail::HttpMultipartPartHeaderStatus::kOk:
                    break;
                case detail::HttpMultipartPartHeaderStatus::kInvalidDisposition:
                    throw std::invalid_argument("invalid multipart content disposition");
                case detail::HttpMultipartPartHeaderStatus::kMissingName:
                    throw std::invalid_argument("invalid multipart field name");
            }

            currentName_.assign(partHeaders.name.data(), partHeaders.name.size());
            currentFilename_.clear();
            currentContentType_.clear();
            if (!partHeaders.filename.empty()) {
                currentFilename_.assign(partHeaders.filename.data(), partHeaders.filename.size());
            }
            if (!partHeaders.contentType.empty()) {
                currentContentType_.assign(partHeaders.contentType.data(), partHeaders.contentType.size());
            }
            consume(headersEnd + 4);
            partBegin_ = true;
            state_ = State::kBody;
            co_return;
        }
    }

    [[nodiscard]] MultipartStreamPart makePart(std::string_view body, bool partEnd) {
        MultipartStreamPart part;
        part.name = currentName_;
        part.filename = currentFilename_;
        part.contentType = currentContentType_;
        part.body = body;
        part.partBegin = partBegin_;
        part.partEnd = partEnd;
        partBegin_ = false;
        return part;
    }

    Task<std::optional<MultipartStreamPart>> readBodyChunk() {
        for (;;) {
            const auto buffer = bufferView();
            const auto boundary = buffer.find(boundary_.prefix());
            if (boundary != std::string_view::npos) {
                if (boundary > 0 || partBegin_) {
                    auto part = makePart(buffer.substr(0, boundary), true);
                    pendingEraseBytes_ = boundary;
                    state_ = State::kBoundary;
                    co_return part;
                }
                state_ = State::kBoundary;
                break;
            }

            const auto keepTail = boundary_.line().size() + 6;
            if (buffer.size() > keepTail) {
                const auto bytes = buffer.size() - keepTail;
                auto part = makePart(buffer.substr(0, bytes), false);
                pendingEraseBytes_ = bytes;
                co_return part;
            }

            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
        }

        co_return std::nullopt;
    }

    BodyReader& bodyReader_;
    std::pmr::memory_resource* resource_;
    std::pmr::string buffer_;
    detail::MultipartBoundaryMarkers boundary_;
    std::pmr::string currentName_;
    std::pmr::string currentFilename_;
    std::pmr::string currentContentType_;
    State state_{State::kBoundary};
    std::size_t bufferOffset_{0};
    std::size_t pendingEraseBytes_{0};
    bool partBegin_{false};
};

}  // namespace ruvia
