#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace ruvia {

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : HttpResponse(detail::HttpResolvedPmrResourceTag{}, detail::httpPmrResourceOrDefault(resource)) {}

HttpResponse::HttpResponse(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : headers_(detail::HttpResolvedPmrResourceTag{}, resource) {}

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
