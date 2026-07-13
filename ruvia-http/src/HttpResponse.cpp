#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/PmrResource.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : HttpResponse(
          detail::HttpResolvedPmrResourceTag{},
          detail::httpPmrResourceOrDefault(resource)) {}

HttpResponse::HttpResponse(
    detail::HttpResolvedPmrResourceTag,
    std::pmr::memory_resource* resource)
    : headers_(detail::HttpResolvedPmrResourceTag{}, resource) {}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return headers_.resource_;
}

std::uint16_t HttpResponse::status() const noexcept {
    return statusCode_;
}

const HttpResponseHeaders& HttpResponse::headers() const noexcept {
    return headers_;
}

void HttpResponse::status(std::uint16_t statusCode) {
    if (statusCode == 101) {
        throw std::invalid_argument(
            "101 Switching Protocols requires a dedicated protocol driver");
    }
    if (!detail::httpFinalStatusCodeValid(statusCode)) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    statusCode_ = statusCode;
}

void HttpResponse::setBodyCopy(std::string_view value) {
    body_.setCopy(resource(), value);
}

void HttpResponse::setBodyView(std::string_view value) noexcept {
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
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setOwnedFile(resource(), file, size, offset, length);
}

void HttpResponse::setBorrowedFileBody(const std::filesystem::path& file, std::uint64_t size) {
    setBorrowedFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedFileBody(
    const std::filesystem::path& file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file.c_str(), size, offset, length);
}

void HttpResponse::setBorrowedNativeFileBody(
    const detail::HttpNativePathChar* file,
    std::uint64_t size) {
    setBorrowedNativeFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedNativeFileBody(
    const detail::HttpNativePathChar* file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    if (file == nullptr || *file == detail::HttpNativePathChar{}) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.setBorrowedFile(file, size, offset, length);
}

}  // namespace ruvia
