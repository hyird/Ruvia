#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseStaticHeaders.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include <array>
#include <charconv>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : HttpResponse(detail::HttpResolvedPmrResourceTag{}, detail::httpPmrResourceOrDefault(resource)) {}

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

HttpResponse HttpResponse::cloneForTransaction() const {
    HttpResponse clone(detail::HttpResolvedPmrResourceTag{}, resource());
    clone.statusCode_ = statusCode_;

    clone.headers_.reserve(headers_.size());
    for (const auto& header : headers_) {
        auto copy = clone.headers_.makeOwnedHeader(header.name(), header.value(), header.knownBit);
        detail::setResponseHeaderAppend(copy, detail::responseHeaderAppend(header));
        (void)clone.headers_.appendPreparedHeader(copy);
    }
    clone.knownHeaderBits_ = knownHeaderBits_;
    clone.knownHeaderIndexes_ = knownHeaderIndexes_;

    if (const auto* const borrowedBytes = body_.borrowedBytes()) {
        clone.body_.setBorrowed(borrowedBytes->bytes());
    } else if (const auto* const staticBytes = body_.staticBytes()) {
        clone.body_.setStatic(staticBytes->bytes());
    } else if (const auto* const ownedBytes = body_.ownedBytes()) {
        clone.body_.setCopy(clone.resource(), ownedBytes->bytes());
    } else if (const auto* const ownedFile = body_.ownedFile()) {
        clone.body_.setOwnedFile(
            clone.resource(),
            detail::makePathFromHttpNativePath(ownedFile->nativePathCStr()),
            ownedFile->size(),
            ownedFile->offset(),
            ownedFile->length(),
            ownedFile->identity());
    } else if (const auto* const borrowedFile = body_.borrowedFile()) {
        clone.body_.setBorrowedFile(borrowedFile->nativePathCStr(), borrowedFile->size(), borrowedFile->offset(), borrowedFile->length(), borrowedFile->identity());
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

void HttpResponse::body(std::string_view value) {
    body_.setCopy(resource(), value);
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

void HttpResponse::replaceBodyWithContentEncoding(std::pmr::string&& value, std::string_view contentEncoding) {
    if (contentEncoding.empty()) {
        throw std::invalid_argument("encoded response body requires a content coding");
    }

    constexpr std::size_t kEncodingHeader = 0;
    constexpr std::size_t kEtagHeader = 1;
    constexpr std::size_t kLengthHeader = 2;
    std::array<HttpResponseHeader, 3> prepared{};
    std::array<bool, 3> preparedActive{};
    const auto releasePrepared = [&]() noexcept {
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            if (preparedActive[i]) {
                headers_.releaseHeader(prepared[i]);
                preparedActive[i] = false;
            }
        }
    };

    // Build the weak validator and all replacement descriptors while the
    // response still owns its identity body. Header vector growth is reserved
    // before any descriptor is published; every later commit operation is a
    // descriptor replacement or an append into already-reserved storage.
    std::pmr::string weakEtag(resource());
    const auto currentEtag = knownHeaderValue(detail::kResponseHeaderEtag);
    if (!currentEtag.empty() && currentEtag.front() == '"') {
        weakEtag.reserve(currentEtag.size() + 2);
        weakEtag.append("W/");
        weakEtag.append(currentEtag.data(), currentEtag.size());
    }

    const std::array<std::pair<std::string_view, std::uint32_t>, 3> fields{{
        {"Content-Encoding", detail::kResponseHeaderContentEncoding},
        {"ETag", detail::kResponseHeaderEtag},
        {"Content-Length", detail::kResponseHeaderContentLength},
    }};
    std::size_t missingHeaders = 0;
    for (const auto& [name, knownBit] : fields) {
        if (knownBit == detail::kResponseHeaderEtag && weakEtag.empty()) {
            continue;
        }
        if (findHeaderForRead(name, knownBit) == nullptr) {
            ++missingHeaders;
        }
    }
    headers_.reserve(headers_.size() + missingHeaders);

    try {
        const auto stage = [&](std::size_t slot, std::string_view name, std::string_view fieldValue, std::uint32_t knownBit) {
            const auto builtin = HttpResponseHeaders::makeStaticHeader(name, fieldValue, knownBit);
            prepared[slot] = builtin ? *builtin : headers_.makeOwnedHeader(name, fieldValue, knownBit);
            preparedActive[slot] = true;
        };

        stage(kEncodingHeader, fields[kEncodingHeader].first, contentEncoding, fields[kEncodingHeader].second);
        if (!weakEtag.empty()) {
            stage(kEtagHeader, fields[kEtagHeader].first, weakEtag, fields[kEtagHeader].second);
        }

        std::array<char, 32> lengthBuffer{};
        const auto [lengthEnd, lengthError] = std::to_chars(lengthBuffer.data(), lengthBuffer.data() + lengthBuffer.size(), value.size());
        if (lengthError != std::errc{}) {
            throw std::logic_error("failed to format encoded response length");
        }
        stage(kLengthHeader, fields[kLengthHeader].first, std::string_view(lengthBuffer.data(), static_cast<std::size_t>(lengthEnd - lengthBuffer.data())), fields[kLengthHeader].second);

        // The encoded bytes use this response's resource. setOwned constructs
        // the new body alternative before replacing the old variant, so a
        // resource failure still leaves the identity body and old headers in
        // place. Header commits below are no-throw after the reserve/staging
        // phase and therefore form the publication point.
        body_.setOwned(resource(), std::move(value));

        const auto commit = [&](std::size_t slot, std::string_view name, std::uint32_t knownBit) noexcept {
            if (auto* const existing = findHeaderForUpdate(name, knownBit)) {
                const bool wasAppended = detail::responseHeaderAppend(*existing);
                headers_.releaseHeader(*existing);
                *existing = prepared[slot];
                preparedActive[slot] = false;
                if (wasAppended) {
                    (void)collapseResponseHeaders(*existing, name, knownBit);
                }
                return;
            }

            const auto index = headers_.size();
            (void)headers_.appendPreparedHeader(prepared[slot]);
            preparedActive[slot] = false;
            recordKnownHeaderIndex(knownBit, index);
        };

        commit(kEncodingHeader, fields[kEncodingHeader].first, fields[kEncodingHeader].second);
        if (!weakEtag.empty()) {
            commit(kEtagHeader, fields[kEtagHeader].first, fields[kEtagHeader].second);
        }
        commit(kLengthHeader, fields[kLengthHeader].first, fields[kLengthHeader].second);
    } catch (...) {
        releasePrepared();
        throw;
    }
}

void HttpResponse::materializeBody() {
    body_.materialize(resource());
}

void HttpResponse::setFileBody(std::filesystem::path file, std::uint64_t size) {
    setFileBody(std::move(file), size, 0, size);
}

void HttpResponse::setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    setFileBody(std::move(file), size, offset, length, detail::ResponseFileIdentity::unchecked());
}

void HttpResponse::setFileBody(std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length, detail::ResponseFileIdentity identity) {
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

void HttpResponse::setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file.c_str(), size, offset, length);
}

void HttpResponse::setBorrowedNativeFileBody(const detail::HttpNativePathChar* file, std::uint64_t size) {
    setBorrowedNativeFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedNativeFileBody(const detail::HttpNativePathChar* file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file == nullptr || *file == detail::HttpNativePathChar{}) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file, size, offset, length);
}

}  // namespace ruvia
