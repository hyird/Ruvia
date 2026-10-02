#include "ruvia/web/detail/http3/Http3ClientRequestWrite.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3RequestWriter.h"
#include "ruvia/web/detail/client/HttpClientTunnelState.h"
#include "ruvia/web/detail/client/HttpClientUploadState.h"
#include "ruvia/web/detail/http3/Http3ClientSansIoSessionEngine.h"

namespace ruvia::detail {

using PreparedRequestWriteResult = std::expected<Http3ClientRequestWrite,
    Http3ClientRequestWriteError>;
static_assert(std::is_nothrow_constructible_v<PreparedRequestWriteResult, std::in_place_t,
    Http3ClientRequestWrite::PreparedTag, std::pmr::memory_resource*, HttpClientRequestStorage&&,
    std::pmr::string&&, std::pmr::string&&, std::pmr::vector<char>&&, Http3DataWritePlan>);

Http3ClientRequestWrite::Http3ClientRequestWrite(PreparedTag,
    std::pmr::memory_resource* resource, HttpClientRequestStorage&& request,
    std::pmr::string&& scheme,
    std::pmr::string&& authority, std::pmr::vector<char>&& headers,
    Http3DataWritePlan dataPlan, Http3FieldSectionLimits limits) noexcept
    : workerPool_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      fieldLimits_(limits),
      request_(std::move(request)),
      scheme_(std::move(scheme)),
      authority_(std::move(authority)),
      headers_(std::move(headers)),
      dataPlan_(std::move(dataPlan)) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(request_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(scheme_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(authority_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<Http3DataWritePlan>);
    static_assert(std::is_nothrow_constructible_v<decltype(dataPlan_), Http3DataWritePlan&&>);
}

Http3ClientRequestWrite& Http3ClientRequestWrite::requireNoOutstandingSegment(
    Http3ClientRequestWrite& other) {
    if (other.offered_) {
        throw std::logic_error("cannot move an HTTP/3 request cursor with an outstanding span");
    }
    return other;
}

Http3ClientRequestWrite::Http3ClientRequestWrite(Http3ClientRequestWrite&& other)
    : workerPool_(requireNoOutstandingSegment(other).workerPool_),
      fieldLimits_(other.fieldLimits_),
      request_(std::move(other.request_)),
      scheme_(std::move(other.scheme_), workerPool_),
      authority_(std::move(other.authority_), workerPool_),
      headers_(std::move(other.headers_), workerPool_),
      dataPlan_(std::move(other.dataPlan_)),
      chunk_(other.chunk_),
      segmentOffset_(other.segmentOffset_),
      bodyOffset_(other.bodyOffset_),
      state_(other.state_),
      chunkPending_(other.chunkPending_),
      requestTaken_(other.requestTaken_) {
    if (chunkPending_) {
        chunk_.payload = (request_.output() != nullptr ? std::string_view(request_.output()->chunk) : request_.body()).substr(bodyOffset_, chunk_.payload.size());
    }
    other.state_ = State::kFailed;
    other.requestTaken_ = true;
}

std::optional<HttpClientRequestStorage> Http3ClientRequestWrite::takeRequestAfterRetirement() {
    if (requestTaken_) {
        return std::nullopt;
    }
    state_ = State::kFailed;
    offered_ = false;
    chunkPending_ = false;
    chunk_ = {};
    dataPlan_.reset();
    requestTaken_ = true;
    return std::optional<HttpClientRequestStorage>(std::in_place, std::move(request_));
}

std::expected<Http3ClientRequestWrite, Http3ClientRequestWrite::Error>
Http3ClientRequestWrite::create(HttpClientRequestStorage&& request, std::string_view scheme,
    std::string_view authority, std::pmr::memory_resource* workerPool,
    Http3FieldSectionLimits limits) noexcept {
    auto* resource = workerPool != nullptr ? workerPool : std::pmr::get_default_resource();
    try {
        std::optional<HttpClientRequestStorage> normalizedRequest;
        const HttpClientRequestStorage* preparedRequest = &request;
        if (request.resource() != resource) {
            // intoResource copies when resources differ and leaves its source
            // untouched if any normalization allocation fails.
            normalizedRequest.emplace(std::move(request).intoResource(resource));
            preparedRequest = &*normalizedRequest;
        }

        std::pmr::string ownedScheme(scheme, resource);
        std::pmr::string ownedAuthority(authority, resource);
        std::pmr::vector<Http3FieldSectionFieldView> fields(resource);
        fields.reserve(HttpClientRequestStorageAccess::headers(*preparedRequest).size());
        for (const auto& field : HttpClientRequestStorageAccess::headers(*preparedRequest)) {
            fields.push_back({field.name, field.value, false});
        }
        const auto* upload = preparedRequest->upload();
        if (upload != nullptr && upload->config.expectation == HttpClientRequestExpectation::kContinue) {
            if (std::ranges::any_of(fields, [](const auto& field) { return field.name == "expect"; })) {
                return std::unexpected(Error::kInvalidRequest);
            }
            fields.push_back({"expect", "100-continue", false});
        }
        const auto target = preparedRequest->target();
        if (preparedRequest->method() == "CONNECT" && !preparedRequest->isTunnel()) {
            return std::unexpected(Error::kUnsupportedTunnel);
        }
        if (!preparedRequest->isTunnel() && (target.empty() || (target.front() != '/' && target != "*") || target.find('#') != std::string_view::npos)) {
            return std::unexpected(Error::kInvalidRequest);
        }
        if (preparedRequest->isTunnel()) {
            ownedAuthority.assign(preparedRequest->tunnelAuthority());
        }
        // Request framing is method-independent (RFC 9110 section 9.3.2).
        // HEAD suppresses response payload, not explicitly supplied request
        // content. The caller must have established that its origin supports
        // HEAD content; requests without supplied content remain bodyless.
        const bool sendsBody = HttpClientRequestStorageAccess::hasBody(*preparedRequest);
        const bool hasDeclaredLength = std::any_of(fields.begin(), fields.end(), [](const auto& field) {
            constexpr std::string_view name = "content-length";
            if (field.name.size() != name.size()) {
                return false;
            }
            for (std::size_t i = 0; i < name.size(); ++i) {
                const auto ch = static_cast<unsigned char>(field.name[i]);
                if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != name[i]) {
                    return false;
                }
            }
            return true;
        });
        // An explicitly declared length on a bodyless request must agree with
        // the actual zero-byte FIN, without adding Content-Length: 0 to every
        // GET/HEAD lacking that field.
        const std::optional<std::uint64_t> bodyLength = upload != nullptr ? upload->config.contentLength : sendsBody       ? std::optional<std::uint64_t>(preparedRequest->body().size())
                                                                                                       : hasDeclaredLength ? std::optional<std::uint64_t>(0)
                                                                                                                           : std::nullopt;
        // Extended CONNECT cannot be encoded before received SETTINGS authorize
        // it. Keep an unbounded cursor with no provisional wire head; the sole
        // connection driver encodes and validates it before the first write.
        auto encoded = [&]() -> std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> {
            if (preparedRequest->isTunnel() && !preparedRequest->tunnelProtocol().empty()) {
                return Http3ClientRequestHead(resource);
            }
            return encodeHttp3ClientRequestHead({preparedRequest->method(), preparedRequest->isTunnel() ? std::string_view{} : std::string_view(ownedScheme), ownedAuthority, target, fields, bodyLength}, limits, resource);
        }();
        if (!encoded) {
            return std::unexpected(Error::kRequestEncoding);
        }
        if (encoded->fieldSection.size() > std::numeric_limits<std::size_t>::max() - kHttp3FrameHeaderMaxBytes) {
            return std::unexpected(Error::kRequestEncoding);
        }
        std::pmr::vector<char> headers(resource);
        headers.resize(kHttp3FrameHeaderMaxBytes + encoded->fieldSection.size());
        auto frame = encodeHttp3FrameHeader(headers,
            static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->fieldSection.size());
        if (!frame) {
            return std::unexpected(Error::kRequestEncoding);
        }
        headers.resize(*frame + encoded->fieldSection.size());
        std::copy(encoded->fieldSection.begin(), encoded->fieldSection.end(),
            headers.begin() + static_cast<std::ptrdiff_t>(*frame));

        Http3DataWritePlan dataPlan(encoded->bodyPlan);
        if (normalizedRequest) {
            // All fallible work is complete. Retire the original request only
            // at this commit point; the cursor owns its normalized copy.
            HttpClientRequestStorage retired(std::move(request));
            return std::expected<Http3ClientRequestWrite, Error>(std::in_place, PreparedTag{}, resource,
                std::move(*normalizedRequest), std::move(ownedScheme), std::move(ownedAuthority),
                std::move(headers), std::move(dataPlan), limits);
        }
        return std::expected<Http3ClientRequestWrite, Error>(std::in_place, PreparedTag{}, resource,
            std::move(request), std::move(ownedScheme), std::move(ownedAuthority),
            std::move(headers), std::move(dataPlan), limits);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::kOutOfMemory);
    } catch (...) {
        return std::unexpected(Error::kRequestEncoding);
    }
}

bool Http3ClientRequestWrite::prepareConnectionHead(std::uint64_t streamId, Http3ClientSansIoSessionEngine& engine) {
    if (state_ != State::kHeaders || offered_ || segmentOffset_ != 0) {
        return false;
    }
    if (engine.peerSettings() && engine.peerSettings()->maxFieldSectionSize) {
        fieldLimits_.maxDecodedBytes = static_cast<std::size_t>(std::min<std::uint64_t>(fieldLimits_.maxDecodedBytes, *engine.peerSettings()->maxFieldSectionSize));
    }
    if (!request_.isTunnel() && (!engine.peerSettings() || (engine.peerSettings()->qpackMaxTableCapacity == 0 && !engine.peerSettings()->maxFieldSectionSize))) {
        return true;
    }
    std::pmr::vector<Http3FieldSectionFieldView> fields(workerPool_);
    for (const auto& field : HttpClientRequestStorageAccess::headers(request_)) {
        fields.push_back({field.name, field.value, false});
    }
    if (request_.upload() != nullptr && request_.upload()->config.expectation == HttpClientRequestExpectation::kContinue) {
        fields.push_back({"expect", "100-continue", false});
    }
    const auto length = request_.upload() != nullptr ? request_.upload()->config.contentLength : HttpClientRequestStorageAccess::hasBody(request_) ? std::optional<std::uint64_t>{request_.body().size()}
                                                                                                                                                   : std::nullopt;
    const auto encoded = engine.encodeRequestHead(streamId, {.method = request_.method(),
                                                                .scheme = request_.isTunnel() && request_.tunnelProtocol().empty() ? std::string_view{} : std::string_view(scheme_),
                                                                .authority = authority_,
                                                                .path = request_.target(),
                                                                .fields = fields,
                                                                .bodyLength = length,
                                                                .protocol = request_.tunnelProtocol(),
                                                                .peerEnableConnectProtocol = engine.peerSettings() && engine.peerSettings()->enableConnectProtocol});
    if (!encoded) {
        return false;
    }
    headers_.resize(kHttp3FrameHeaderMaxBytes + encoded->fieldSection.size());
    const auto prefix = encodeHttp3FrameHeader(headers_, static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->fieldSection.size());
    if (!prefix) {
        return false;
    }
    headers_.resize(*prefix + encoded->fieldSection.size());
    std::copy(encoded->fieldSection.begin(), encoded->fieldSection.end(), headers_.begin() + static_cast<std::ptrdiff_t>(*prefix));
    return true;
}

std::expected<Http3ClientRequestWrite::Segment, Http3ClientRequestWrite::Error>
Http3ClientRequestWrite::next() noexcept {
    if (state_ == State::kFinished || state_ == State::kFailed) {
        return std::unexpected(Error::kInvalidState);
    }
    if (state_ == State::kFin) {
        return Segment{};
    }
    if (state_ == State::kDataHeader && !chunkPending_) {
        if (auto result = prepareData(); !result) {
            return std::unexpected(result.error());
        }
        if (state_ == State::kFin || (state_ == State::kDataHeader && !chunkPending_)) {
            return Segment{};
        }
    }
    const auto segment = activeSegment();
    offered_ = !segment.empty();
    return segment;
}

Http3ClientRequestWrite::Segment Http3ClientRequestWrite::activeSegment() const noexcept {
    switch (state_) {
        case State::kHeaders:
        case State::kTrailers:
            return Segment(headers_).subspan(segmentOffset_);
        case State::kDataHeader:
            return Segment(chunk_.frameHeader.data(), chunk_.frameHeaderSize).subspan(segmentOffset_);
        case State::kDataBody:
            return chunk_.payload.subspan(segmentOffset_);
        default:
            return {};
    }
}

std::expected<void, Http3ClientRequestWrite::Error> Http3ClientRequestWrite::prepareData() noexcept {
    if (!dataPlan_ || state_ != State::kDataHeader || chunkPending_) {
        return std::unexpected(Error::kInvalidState);
    }
    auto* upload = request_.output();
    if (upload != nullptr) {
        if (upload->stopped || (request_.tunnel() != nullptr ? !request_.tunnel()->accepted : !request_.upload()->contentReleased) || (!upload->chunkReady && !upload->endRequested)) {
            return {};
        }
        if (upload->endRequested && !upload->chunkReady) {
            auto ending = dataPlan_->planChunk(std::span<const char>{}, true);
            if (!ending) {
                return failPlan();
            }
            chunk_ = *ending;
            chunkPending_ = true;
            segmentOffset_ = 0;
            if (request_.upload() != nullptr && !request_.upload()->trailers.empty()) {
                return prepareTrailers();
            }
            state_ = State::kFin;
            return {};
        }
    }
    const auto body = upload != nullptr ? std::string_view(upload->chunk) : request_.body();
    if (bodyOffset_ > body.size()) {
        return failPlan();
    }
    const auto remaining = body.size() - bodyOffset_;
    const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kHttp3VarIntMax));
    const bool finishing = upload == nullptr && size == remaining;
    auto planned = dataPlan_->planChunk(body.substr(bodyOffset_, size), finishing);
    if (!planned) {
        return failPlan();
    }
    chunk_ = *planned;
    chunkPending_ = true;
    segmentOffset_ = 0;
    if (!chunk_.emitsData) {
        state_ = State::kFin;
    }
    return {};
}

std::expected<void, Http3ClientRequestWrite::Error>
Http3ClientRequestWrite::acknowledge(std::size_t count) noexcept {
    if (!offered_ || state_ == State::kFinished || state_ == State::kFailed || state_ == State::kFin) {
        return std::unexpected(Error::kInvalidState);
    }
    const auto segment = activeSegment();
    if (count > segment.size()) {
        return std::unexpected(Error::kExcessiveAcknowledgement);
    }
    if (count == 0) {
        return {};
    }
    segmentOffset_ += count;
    // segment is already the unacknowledged suffix. Compare the accepted
    // amount with that suffix, not the cumulative offset from its beginning.
    if (count != segment.size()) {
        return {};
    }
    offered_ = false;
    segmentOffset_ = 0;
    if (state_ == State::kHeaders) {
        state_ = State::kDataHeader;
    } else if (state_ == State::kTrailers) {
        state_ = State::kFin;
    } else if (state_ == State::kDataHeader) {
        state_ = State::kDataBody;
    } else if (state_ == State::kDataBody) {
        if (chunk_.finishing) {
            state_ = State::kFin;
        } else {
            const auto bytes = chunk_.payload.size();
            if (!dataPlan_->commitPayload(bytes, false)) {
                return failPlan();
            }
            if (auto* upload = request_.output()) {
                upload->acknowledgeChunk();
                bodyOffset_ = 0;
            } else {
                bodyOffset_ += bytes;
            }
            chunkPending_ = false;
            state_ = State::kDataHeader;
        }
    } else {
        return std::unexpected(Error::kInvalidState);
    }
    return {};
}

std::expected<void, Http3ClientRequestWrite::Error>
Http3ClientRequestWrite::acknowledgeFin(bool successful) noexcept {
    if (state_ != State::kFin || !chunkPending_ || !dataPlan_) {
        return std::unexpected(Error::kInvalidState);
    }
    if (!successful) {
        state_ = State::kFailed;
        chunkPending_ = false;
        return {};
    }
    if (!dataPlan_->commitPayload(chunk_.payload.size(), true)) {
        return failPlan();
    }
    state_ = State::kFinished;
    if (auto* upload = request_.output()) {
        upload->finish();
    }
    chunkPending_ = false;
    return {};
}

std::expected<void, Http3ClientRequestWrite::Error> Http3ClientRequestWrite::prepareTrailers() noexcept {
    try {
        std::pmr::vector<Http3FieldSectionFieldView> fields(workerPool_);
        for (const auto& field : request_.upload()->trailers) {
            fields.push_back({field.name(), field.value(), false});
        }
        auto encoded = encodeHttp3RequestTrailers(fields, fieldLimits_, workerPool_);
        if (!encoded) {
            return failPlan();
        }
        std::pmr::vector<char> frame(workerPool_);
        frame.resize(kHttp3FrameHeaderMaxBytes + (*encoded).size());
        const auto prefix = encodeHttp3FrameHeader(frame, static_cast<std::uint64_t>(Http3FrameType::kHeaders), (*encoded).size());
        if (!prefix) {
            return failPlan();
        }
        frame.resize(*prefix + (*encoded).size());
        std::copy((*encoded).begin(), (*encoded).end(), frame.begin() + static_cast<std::ptrdiff_t>(*prefix));
        headers_.swap(frame);
        state_ = State::kTrailers;
        return {};
    } catch (...) {
        return failPlan();
    }
}

bool Http3ClientRequestWrite::waitingForContent() const noexcept {
    const auto* output = request_.output();
    return output != nullptr && state_ == State::kDataHeader && !chunkPending_ &&
           ((request_.tunnel() != nullptr ? !request_.tunnel()->accepted : !request_.upload()->contentReleased) || (!output->chunkReady && !output->endRequested));
}
void Http3ClientRequestWrite::stopSending() noexcept {
    state_ = State::kFinished;
    offered_ = false;
    chunkPending_ = false;
    dataPlan_.reset();
}

bool Http3ClientRequestWrite::finReady() const noexcept {
    return state_ == State::kFin;
}
bool Http3ClientRequestWrite::finished() const noexcept {
    return state_ == State::kFinished;
}
bool Http3ClientRequestWrite::failed() const noexcept {
    return state_ == State::kFailed;
}

HttpKnownMethod Http3ClientRequestWrite::knownMethod() const noexcept {
    return classifyHttpMethod(request_.method());
}

std::expected<void, Http3ClientRequestWrite::Error> Http3ClientRequestWrite::failPlan() noexcept {
    state_ = State::kFailed;
    chunkPending_ = false;
    return std::unexpected(Error::kDataPlan);
}

}  // namespace ruvia::detail
