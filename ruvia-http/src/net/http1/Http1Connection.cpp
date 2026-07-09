#include "Http1Connection.h"

#include <algorithm>

#include "Http1ServerSemantics.h"
#include "../../parser/HttpChunkParser.h"
#include "../../parser/HttpParserSyntax.h"

namespace ruvia::detail {

Http1Connection::Http1Connection(std::pmr::memory_resource* resource, Http1CoreConfig config)
    : config_(config), input_(resource), headStorage_(resource), events_(resource) {}

Http1FeedResult Http1Connection::feed(std::string_view in) {
    // Same per-feed view contract as Http2Connection: the previous feed's event views
    // die now (append may reallocate), so the owner drained them already.
    events_.clear();
    eventOffset_ = 0;
    if (state_ == State::kError) {
        return {0, Http1FeedStatus::kError};
    }
    // Reclaim the body prefix consumed by the PREVIOUS feed now (h2-style: at the start
    // of the NEXT feed, so this feed's body-chunk views stayed valid until now). The
    // head lives in the stable headStorage_ (see advance()), so head() views survive
    // this reclaim -- and a content-length/streaming body no longer accumulates the
    // whole message in input_ (previously unbounded with maxBodyBytes=0).
    if (inBodyState() && cursor_ > 0) {
        input_.erase(0, cursor_);
        cursor_ = 0;
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
    // RFC 9112 §9.3 persistence -- single-sourced in Http1ServerSemantics.h (also used
    // by the web h1 session), since it is pure protocol semantics.
    return http1ShouldKeepAlive(parsed_);
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
    input_.erase(0, cursor_);  // drop this message's trailing bytes; keep pipelined input
    cursor_ = 0;
    headStorage_.clear();
    headerSearchOffset_ = 0;
    scanResume_ = 0;
    bodyRemaining_ = 0;
    decodedBodyBytes_ = 0;
    error_ = HttpParseError::kNone;
    state_ = State::kHead;
    advance();  // pipelined requests: parse what is already buffered
}

bool Http1Connection::inBodyState() const noexcept {
    switch (state_) {
        case State::kBodyContentLength:
        case State::kChunkSize:
        case State::kChunkData:
        case State::kChunkDataCrlf:
        case State::kChunkTrailers:
            return true;
        default:
            return false;
    }
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
                // Stabilize the head: parsed_.request holds views into input_, but a
                // later feed's append (or the body reclaim above) can reallocate/shift
                // input_ and dangle them. Copy the head bytes into headStorage_ and
                // re-parse into that stable buffer so head() stays valid until
                // nextMessage(), then drop the head from input_ (cursor_ back to 0, so
                // body/pipelined bytes are indexed from the front).
                headStorage_.assign(input_.data(), parsed_.headerBytes);
                std::size_t stableOffset = 0;
                parser_.parseHeaders(
                    std::string_view(headStorage_.data(), headStorage_.size()), parsed_,
                    stableOffset);
                input_.erase(0, parsed_.headerBytes);
                cursor_ = 0;
                events_.push_back(Http1Event{Http1Event::Kind::kMessageHead, {}});
                if (parsed_.chunked) {
                    scanResume_ = 0;
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
                // Resume the "\r\n" search where the last feed left off (scanResume_ is
                // relative to `available`) instead of re-scanning the whole accumulating
                // line -- otherwise a chunk-size line trickled byte-by-byte is O(n^2).
                const auto found = available.find("\r\n", scanResume_);
                if (found == std::string_view::npos) {
                    if (available.size() > config_.maxHeaderBytes) {
                        fail(HttpParseError::kInvalidChunkSize);
                        return;
                    }
                    scanResume_ = available.size() >= 1 ? available.size() - 1 : 0;
                    return;
                }
                scanResume_ = 0;
                const auto lineEnd = found;
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
                    scanResume_ = 0;
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
                scanResume_ = 0;
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
                const auto terminator = available.find("\r\n\r\n", scanResume_);
                if (terminator == std::string_view::npos) {
                    scanResume_ = available.size() >= 3 ? available.size() - 3 : 0;
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
