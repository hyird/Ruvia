#include "ruvia/http/HttpResponse.h"

#include <array>
#include <charconv>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include "coding/HttpContentCoding.h"
#include "field/HttpEntityTag.h"
#include "response/HttpResponseHeaderAccess.h"
#include "response/HttpResponseStaticHeaders.h"
#include "response/ResponseHeaderUtils.h"

namespace ruvia {

std::string_view HttpResponse::bodyBytes() const& noexcept {
    return detail::responseBody(*this).bytes();
}

std::optional<HttpResponseFileView> HttpResponse::fileBody() const& noexcept {
    return detail::responseBody(*this).file();
}

bool HttpResponse::has_multipart_file_body() const noexcept {
    return detail::responseBody(*this).multipart_body() != nullptr;
}

std::size_t HttpResponse::body_segment_count() const noexcept {
    const auto& body = detail::responseBody(*this);
    if (const auto* multipart = body.multipart_body()) {
        return multipart->segment_count();
    }
    return body.size() == 0 ? 0 : 1;
}

http_response_body_segment_view HttpResponse::body_segment(std::size_t index) const& {
    const auto& body = detail::responseBody(*this);
    if (const auto* multipart = body.multipart_body()) {
        if (index >= multipart->segment_count()) {
            throw std::out_of_range("response body segment index is out of range");
        }
        return multipart->segment(index);
    }
    if (index != 0) {
        throw std::out_of_range("response body segment index is out of range");
    }
    if (const auto file = body.file()) {
        return {.file_ = file};
    }
    return {.bytes_ = body.bytes()};
}

std::uint64_t HttpResponseBodyPlan::bufferedRepresentationLength(const HttpResponse& response) const noexcept {
    if (!statusAllowsBody() || contentSemantics() == HttpResponseContentSemantics::kConnectTunnel) {
        return 0;
    }
    return static_cast<std::uint64_t>(detail::responseBody(response).size());
}

bool HttpBufferedResponseWritePlan::matchesResponse(const HttpResponse& response) const noexcept {
    return response.status() == bodyPlan_.responseStatus() &&
           contentLength_ == bodyPlan_.bufferedRepresentationLength(response);
}

HttpBufferedResponseWritePlan planBufferedHttpResponseWrite(
    HttpKnownMethod requestMethod, const HttpResponse& response) noexcept {
    const auto bodyPlan = planHttpResponseBody(requestMethod, response.status());
    return HttpBufferedResponseWritePlan(bodyPlan, bodyPlan.bufferedRepresentationLength(response));
}

HttpResponseBodyPlan planHttpResponseBody(
    HttpKnownMethod requestMethod, HttpStatusCode responseStatus) noexcept {
    const auto policy = detail::responseWritePolicy(responseStatus);
    const auto semantics = detail::httpResponseContentSemantics(requestMethod, responseStatus);
    const bool connectTunnel = semantics == HttpResponseContentSemantics::kConnectTunnel;
    return HttpResponseBodyPlan(requestMethod, responseStatus, semantics, policy.bodyAllowed(),
        !policy.bodyAllowed() || semantics != HttpResponseContentSemantics::kWithContent,
        !connectTunnel && policy.autoContentLengthAllowed(), !connectTunnel && policy.explicitContentLengthAllowed(),
        !connectTunnel && policy.transferEncodingAllowed());
}

namespace {

[[nodiscard]] std::pmr::string weakEtagForNewRepresentation(
    std::string_view currentEtag, std::pmr::memory_resource* resource) {
    std::pmr::string weakEtag(resource);
    if (detail::httpIsStrongEtag(currentEtag)) {
        weakEtag.reserve(currentEtag.size() + 2);
        weakEtag.append("W/");
        weakEtag.append(currentEtag.data(), currentEtag.size());
    }
    return weakEtag;
}

}  // namespace

HttpResponse::HttpResponse()
    : HttpResponse(Options{}) {}

HttpResponse::HttpResponse(Options options)
    : HttpResponse(detail::HttpResolvedPmrResourceTag{},
          detail::httpPmrResourceOrDefault(options.resource)) {}

HttpResponse::HttpResponse(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : headers_(detail::HttpResolvedPmrResourceTag{}, resource) {}

HttpResponse::HttpResponse(HttpResponse&& other) noexcept
    : statusCode_(other.statusCode_),
      knownHeaderBits_(other.knownHeaderBits_),
      knownHeaderIndexes_(other.knownHeaderIndexes_),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)) {
    other.knownHeaderBits_ = 0;
    other.knownHeaderIndexes_.fill(0);
}

HttpResponse& HttpResponse::operator=(HttpResponse&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    // A response is one resource domain. Member-wise assignment would retain the
    // target allocator in PMR alternatives while HttpResponseHeaders follows the
    // source resource, leaving one response split across unrelated request arenas.
    // Reconstructing transfers every owning alternative together and does not
    // allocate on the response hot path.
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return headers_.resource_;
}

std::pmr::memory_resource* HttpResponse::memoryResource() const noexcept {
    return resource();
}

HttpResponse HttpResponse::cloneHeadersForTransaction(std::size_t additionalHeaders) const {
    HttpResponse clone(detail::HttpResolvedPmrResourceTag{}, resource());
    clone.statusCode_ = statusCode_;

    clone.headers_.reserve(headers_.size() + additionalHeaders);
    for (const auto& header : headers_) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either response may be changed or destroyed.
        auto copy = header.owned
                        ? clone.headers_.makeOwnedHeader(header.name(), header.value(), header.knownBit)
                        : header;
        detail::setResponseHeaderAppend(copy, detail::responseHeaderAppend(header));
        (void)clone.headers_.appendPreparedHeader(copy);
    }
    clone.knownHeaderBits_ = knownHeaderBits_;
    clone.knownHeaderIndexes_ = knownHeaderIndexes_;

    return clone;
}

void HttpResponse::commitHeadersFrom(HttpResponse&& staged) noexcept {
    std::destroy_at(&headers_);
    ::new (static_cast<void*>(&headers_)) HttpResponseHeaders(std::move(staged.headers_));
    knownHeaderBits_ = staged.knownHeaderBits_;
    knownHeaderIndexes_ = staged.knownHeaderIndexes_;
}

HttpResponse HttpResponse::cloneForTransaction() const {
    auto clone = cloneHeadersForTransaction();

    if (const auto* const borrowedBytes = body_.borrowedBytes()) {
        clone.body_.setBorrowed(borrowedBytes->bytes());
    } else if (const auto* const staticBytes = body_.staticBytes()) {
        clone.body_.setStatic(staticBytes->bytes());
    } else if (const auto* const ownedBytes = body_.ownedBytes()) {
        clone.body_.setCopy(clone.resource(), ownedBytes->bytes());
    } else if (const auto* const ownedFile = body_.ownedFile()) {
        clone.body_.setOwnedFile(clone.resource(),
            detail::makePathFromHttpNativePath(ownedFile->nativePathCStr()), ownedFile->size(),
            ownedFile->offset(), ownedFile->length(), ownedFile->identity());
    } else if (const auto* const borrowedFile = body_.borrowedFile()) {
        clone.body_.setBorrowedFile(borrowedFile->nativePathCStr(), borrowedFile->size(),
            borrowedFile->offset(), borrowedFile->length(), borrowedFile->identity());
    } else if (const auto* const multipart = body_.multipart_body()) {
        clone.body_.set_multipart(clone.resource(),
            multipart->file().toPath(), multipart->file_size(),
            multipart->identity(), multipart->plan().clone(clone.resource()));
    }

    return clone;
}

HttpStatusCode HttpResponse::status() const noexcept {
    return statusCode_;
}

const HttpResponseHeaders& HttpResponse::headers() const& noexcept {
    return headers_;
}

void HttpResponse::status(HttpStatusCode statusCode) {
    if (statusCode == http_status::kSwitchingProtocols) {
        throw std::invalid_argument("Switching Protocols requires a dedicated protocol driver");
    }
    if (!detail::httpFinalStatusCodeValid(statusCode)) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    statusCode_ = statusCode;
}

void HttpResponse::transferHeadersFrom(
    const HttpResponse& source, HttpResponseHeaderTransfer mode) {
    auto staged = cloneHeadersForTransaction(source.headers().size());
    bool removedCookies = false;
    for (const auto& header : source.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (mode == HttpResponseHeaderTransfer::kAssign &&
            knownBit == detail::kResponseHeaderContentType) {
            continue;
        }
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie) {
            if (mode == HttpResponseHeaderTransfer::kAssign && !removedCookies) {
                staged.removeHeader("Set-Cookie");
                removedCookies = true;
            }
            staged.header(name, value, {.mode = HttpResponseHeaderMode::kAppend});
        } else if (detail::responseHeaderAppend(header)) {
            const auto occurrenceThrough = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source.headers()) {
                    if (httpAsciiEqualsIgnoreCase(candidate.name(), name) && candidate.value() == value) {
                        ++count;
                    }
                    if (&candidate == &header) {
                        break;
                    }
                }
                return count;
            }();
            const auto existingCount = [&] {
                std::size_t count = 0;
                for (const auto& candidate : staged.headers()) {
                    if (httpAsciiEqualsIgnoreCase(candidate.name(), name) && candidate.value() == value) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existingCount < occurrenceThrough) {
                staged.header(name, value, {.mode = HttpResponseHeaderMode::kAppend});
            }
        } else if (mode == HttpResponseHeaderTransfer::kMerge) {
            if (!staged.header(name)) {
                staged.header(name, value);
            }
        } else {
            staged.header(name, value);
        }
    }
    commitHeadersFrom(std::move(staged));
}

void HttpResponse::body(std::string_view value) {
    body_.setCopy(resource(), value);
}

void HttpResponse::ownedBody(std::pmr::string&& value) {
    setBodyOwned(std::move(value));
}

void HttpResponse::staticBody(std::string_view value) noexcept {
    setBodyStaticView(value);
}

void HttpResponse::setBodyBorrowedView(std::string_view value) noexcept {
    body_.setBorrowed(value);
}

void HttpResponse::setBodyStaticView(std::string_view value) noexcept {
    body_.setStatic(value);
}

void HttpResponse::setBodyOwned(std::pmr::string&& value) {
    body_.setOwned(resource(), std::move(value));
}

// Owns only newly staged descriptors, never copies the response's existing
// header block. Publication transfers descriptors; unwinding releases the rest.
class HttpResponse::encoded_header_update final {
public:
    explicit encoded_header_update(HttpResponse& owner) noexcept
        : owner_(owner) {}
    encoded_header_update(const encoded_header_update&) = delete;
    encoded_header_update& operator=(const encoded_header_update&) = delete;

    ~encoded_header_update() noexcept {
        for (std::size_t slot = 0; slot < prepared_.size(); ++slot) {
            if (active_[slot]) {
                owner_.headers_.releaseHeader(prepared_[slot]);
            }
        }
    }

    void stage(std::size_t slot, std::string_view name, std::string_view value,
        std::uint32_t known_bit) {
        const auto builtin = HttpResponseHeaders::makeStaticHeader(name, value, known_bit);
        prepared_[slot] = builtin ? *builtin : owner_.headers_.makeOwnedHeader(name, value, known_bit);
        active_[slot] = true;
    }

    void commit(std::size_t slot, std::string_view name, std::uint32_t known_bit) noexcept {
        if (auto* const existing = owner_.findHeaderForUpdate(name, known_bit)) {
            const bool was_appended = detail::responseHeaderAppend(*existing);
            owner_.headers_.releaseHeader(*existing);
            *existing = prepared_[slot];
            active_[slot] = false;
            if (was_appended) {
                (void)owner_.collapseResponseHeaders(*existing, known_bit);
            }
            return;
        }
        const auto index = owner_.headers_.size();
        (void)owner_.headers_.appendPreparedHeader(prepared_[slot]);
        active_[slot] = false;
        owner_.recordKnownHeaderIndex(known_bit, index);
    }

private:
    HttpResponse& owner_;
    std::array<HttpResponseHeader, 3> prepared_{};
    std::array<bool, 3> active_{};
};

void HttpResponse::applyContentEncoding(std::string_view contentEncoding) {
    apply_encoded_representation(contentEncoding, nullptr);
}

void HttpResponse::replaceBodyWithContentEncoding(
    std::pmr::string&& value, std::string_view contentEncoding) {
    apply_encoded_representation(contentEncoding, &value);
}

void HttpResponse::apply_encoded_representation(
    std::string_view content_encoding, std::pmr::string* body) {
    detail::validateResponseHeaderStorageSize(std::string_view("Content-Encoding").size(), content_encoding.size());
    if (!detail::isValidHttpContentEncodingFieldValue(content_encoding, detail::HttpFieldListRole::kSender)) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    constexpr std::size_t encoding_header = 0;
    constexpr std::size_t etag_header = 1;
    constexpr std::size_t length_header = 2;
    const auto weak_etag = weakEtagForNewRepresentation(
        knownHeaderValue(detail::kResponseHeaderEtag), resource());
    const std::array<std::pair<std::string_view, std::uint32_t>, 3> fields{{
        {"Content-Encoding", detail::kResponseHeaderContentEncoding},
        {"ETag", detail::kResponseHeaderEtag},
        {"Content-Length", detail::kResponseHeaderContentLength},
    }};
    const auto selected = [&](std::size_t slot) noexcept {
        return (slot != etag_header || !weak_etag.empty()) &&
               (slot != length_header || body != nullptr);
    };
    std::size_t missing_headers = 0;
    for (std::size_t slot = 0; slot < fields.size(); ++slot) {
        if (selected(slot) && findHeaderForRead(fields[slot].first, fields[slot].second) == nullptr) {
            ++missing_headers;
        }
    }
    headers_.reserve(headers_.size() + missing_headers);

    encoded_header_update update(*this);
    update.stage(encoding_header, fields[encoding_header].first, content_encoding,
        fields[encoding_header].second);
    if (selected(etag_header)) {
        update.stage(etag_header, fields[etag_header].first, weak_etag, fields[etag_header].second);
    }
    if (body != nullptr) {
        std::array<char, 32> length_buffer{};
        const auto [length_end, length_error] = std::to_chars(
            length_buffer.data(), length_buffer.data() + length_buffer.size(), body->size());
        if (length_error != std::errc{}) {
            throw std::logic_error("failed to format encoded response length");
        }
        update.stage(length_header, fields[length_header].first,
            std::string_view(length_buffer.data(), static_cast<std::size_t>(length_end - length_buffer.data())),
            fields[length_header].second);
        // All descriptors exist before replacing the body. setOwned has a strong
        // failure guarantee; everything after it is no-throw publication.
        body_.setOwned(resource(), std::move(*body));
    }
    update.commit(encoding_header, fields[encoding_header].first, fields[encoding_header].second);
    if (body == nullptr) {
        (void)removeHeaderValidated("Content-Length", detail::kResponseHeaderContentLength);
    }
    if (selected(etag_header)) {
        update.commit(etag_header, fields[etag_header].first, fields[etag_header].second);
    }
    if (body != nullptr) {
        update.commit(length_header, fields[length_header].first, fields[length_header].second);
    }
}

void HttpResponse::materializeBody() {
    body_.materialize(resource());
}

void HttpResponse::fileBody(std::filesystem::path file, std::uint64_t size,
    std::uint64_t offset, std::uint64_t length, HttpResponseFileIdentity identity) {
    setFileBody(std::move(file), size, offset, length, identity);
}

void HttpResponse::multipart_file_body(std::filesystem::path file, std::uint64_t size,
    HttpResponseFileIdentity identity, http_multipart_byte_range_plan&& plan) {
    body_.set_multipart(resource(), file, size, identity, std::move(plan));
}

void HttpResponse::contentRange(
    std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    setContentRange(offset, length, size);
}

void HttpResponse::contentRangeUnsatisfied(std::uint64_t size) {
    setContentRangeUnsatisfied(size);
}

void HttpResponse::addVaryToken(std::string_view token) {
    detail::addVaryToken(*this, token);
}

void HttpResponse::setFileBody(std::filesystem::path file, std::uint64_t size) {
    setFileBody(std::move(file), size, 0, size);
}

void HttpResponse::setFileBody(
    std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    setFileBody(std::move(file), size, offset, length, HttpResponseFileIdentity::unchecked());
}

void HttpResponse::setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset,
    std::uint64_t length, HttpResponseFileIdentity identity) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setOwnedFile(resource(), file, size, offset, length, identity);
}

void HttpResponse::setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size) {
    setBorrowedFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size,
    std::uint64_t offset, std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file.c_str(), size, offset, length);
}

void HttpResponse::setBorrowedNativeFileBody(
    const detail::HttpNativePathChar* file, std::uint64_t size) {
    setBorrowedNativeFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedNativeFileBody(const detail::HttpNativePathChar* file,
    std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file == nullptr || *file == detail::HttpNativePathChar{}) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file, size, offset, length);
}

}  // namespace ruvia
