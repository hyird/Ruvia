#include "Http1Connection.h"

#include <algorithm>

#include "../../parser/HttpChunkParser.h"
#include "../../parser/HttpParserSyntax.h"

namespace ruvia::detail {

Http1Connection::Http1Connection(std::pmr::memory_resource* resource, Http1CoreConfig config)
    : config_(config), input_(resource), events_(resource) {}

Http1FeedResult Http1Connection::feed(std::string_view in) {
    // Same per-feed view contract as Http2Connection: the previous feed's event views
    // die now (append may reallocate), so the owner drained them already.
    events_.clear();
    eventOffset_ = 0;
    if (state_ == State::kError) {
        return {0, Http1FeedStatus::kError};
    }
    input_.append(in.data(), in.size());
    advance();
    if (state_ == State::kError) {
        return {in.size(), Http1FeedStatus::kError};
    }
    return {in.size(),
            events_.empty() && state_ != State::kMessageDone ? Http1FeedStatus::kNeedMore
                                                             : Http1FeedStatus::kOk};
}

Http1Event Http1Connection::nextEvent() {
    if (eventOffset_ < events_.size()) {
        return events_[eventOffset_++];
    }
    events_.clear();
    eventOffset_ = 0;
    return {};
}

bool Http1Connection::keepAlive() const noexcept {
    // RFC 9112 §9.3: explicit close wins; explicit keep-alive wins for HTTP/1.0;
    // otherwise persistence is the HTTP/1.1 default. (Same rule the web session's
    // shouldKeepAlive applies -- protocol semantics, so it lives here too.)
    if (parsed_.flags.connectionClose) {
        return false;
    }
    if (parsed_.flags.connectionKeepAlive) {
        return true;
    }
    return parsed_.request.httpVersion() == "HTTP/1.1";
}

std::string_view Http1Connection::unconsumedInput() const noexcept {
    return std::string_view(input_.data() + cursor_, input_.size() - cursor_);
}

void Http1Connection::nextMessage() {
    if (state_ != State::kMessageDone) {
        return;  // nothing finished (misuse-tolerant, like the h2 core's no-op guards)
    }
    events_.clear();
    eventOffset_ = 0;
    input_.erase(0, cursor_);
    cursor_ = 0;
    headerSearchOffset_ = 0;
    bodyRemaining_ = 0;
    decodedBodyBytes_ = 0;
    error_ = HttpParseError::kNone;
    state_ = State::kHead;
    advance();  // pipelined requests: parse what is already buffered
}

void Http1Connection::fail(HttpParseError error) {
    error_ = error;
    state_ = State::kError;
}

void Http1Connection::finishMessage() {
    events_.push_back(Http1Event{Http1Event::Kind::kMessageEnd, {}});
    state_ = State::kMessageDone;
}

bool Http1Connection::accountBody(std::size_t bytes) {
    if (config_.maxBodyBytes != 0 &&
        (decodedBodyBytes_ > config_.maxBodyBytes ||
         bytes > config_.maxBodyBytes - decodedBodyBytes_)) {
        fail(HttpParseError::kBodyTooLarge);
        return false;
    }
    decodedBodyBytes_ += bytes;
    return true;
}

void Http1Connection::advance() {
    for (;;) {
        const auto available = std::string_view(input_.data() + cursor_, input_.size() - cursor_);
        switch (state_) {
            case State::kHead: {
                parser_.parseHeaders(
                    std::string_view(input_.data(), input_.size()), parsed_, headerSearchOffset_);
                if (parsed_.status == HttpParseStatus::kIncomplete) {
                    if (input_.size() > config_.maxHeaderBytes) {
                        fail(HttpParseError::kHeaderTooLarge);
                        return;
                    }
                    // Restart the header-terminator scan near the tail next feed.
                    headerSearchOffset_ = input_.size() > 3 ? input_.size() - 3 : 0;
                    return;
                }
                if (parsed_.status == HttpParseStatus::kError) {
                    fail(parsed_.error);
                    return;
                }
                cursor_ = parsed_.headerBytes;
                events_.push_back(Http1Event{Http1Event::Kind::kMessageHead, {}});
                if (parsed_.chunked) {
                    state_ = State::kChunkSize;
                } else if (parsed_.contentLength > 0) {
                    bodyRemaining_ = parsed_.contentLength;
                    state_ = State::kBodyContentLength;
                } else {
                    finishMessage();
                    return;
                }
                break;
            }
            case State::kBodyContentLength: {
                const auto take = std::min(bodyRemaining_, available.size());
                if (take != 0) {
                    if (!accountBody(take)) {
                        return;
                    }
                    events_.push_back(Http1Event{
                        Http1Event::Kind::kMessageBodyChunk, available.substr(0, take)});
                    cursor_ += take;
                    bodyRemaining_ -= take;
                }
                if (bodyRemaining_ != 0) {
                    return;  // need more bytes
                }
                finishMessage();
                return;
            }
            case State::kChunkSize: {
                const auto lineEnd = available.find("\r\n");
                if (lineEnd == std::string_view::npos) {
                    if (available.size() > config_.maxHeaderBytes) {
                        fail(HttpParseError::kInvalidChunkSize);
                    }
                    return;
                }
                std::size_t chunkSize = 0;
                switch (parseHttpChunkSizeLine(available.substr(0, lineEnd), chunkSize)) {
                    case ChunkSizeLineStatus::kOk:
                        break;
                    case ChunkSizeLineStatus::kInvalidSize:
                        fail(HttpParseError::kInvalidChunkSize);
                        return;
                    case ChunkSizeLineStatus::kOverflow:
                        fail(HttpParseError::kChunkSizeOverflow);
                        return;
                    case ChunkSizeLineStatus::kInvalidExtension:
                        fail(HttpParseError::kInvalidChunkExtension);
                        return;
                }
                cursor_ += lineEnd + 2;
                if (chunkSize == 0) {
                    state_ = State::kChunkTrailers;
                } else {
                    bodyRemaining_ = chunkSize;
                    state_ = State::kChunkData;
                }
                break;
            }
            case State::kChunkData: {
                const auto take = std::min(bodyRemaining_, available.size());
                if (take != 0) {
                    if (!accountBody(take)) {
                        return;
                    }
                    events_.push_back(Http1Event{
                        Http1Event::Kind::kMessageBodyChunk, available.substr(0, take)});
                    cursor_ += take;
                    bodyRemaining_ -= take;
                }
                if (bodyRemaining_ != 0) {
                    return;
                }
                state_ = State::kChunkDataCrlf;
                break;
            }
            case State::kChunkDataCrlf: {
                if (available.size() < 2) {
                    return;
                }
                if (available.substr(0, 2) != "\r\n") {
                    fail(HttpParseError::kInvalidChunkCrlf);
                    return;
                }
                cursor_ += 2;
                state_ = State::kChunkSize;
                break;
            }
            case State::kChunkTrailers: {
                // Trailer section: zero or more header lines, then the blank CRLF.
                if (available.size() >= 2 && available.substr(0, 2) == "\r\n") {
                    cursor_ += 2;
                    finishMessage();
                    return;
                }
                const auto terminator = available.find("\r\n\r\n");
                if (terminator == std::string_view::npos) {
                    if (available.size() > config_.maxHeaderBytes) {
                        fail(HttpParseError::kInvalidTrailer);
                    }
                    return;
                }
                if (validateHttpChunkTrailers(available.substr(0, terminator)) !=
                    HttpChunkScanStatus::kComplete) {
                    fail(HttpParseError::kInvalidTrailer);
                    return;
                }
                cursor_ += terminator + 4;
                finishMessage();
                return;
            }
            case State::kMessageDone:
            case State::kError:
                return;
        }
    }
}

}  // namespace ruvia::detail
