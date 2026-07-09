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
    : statusText_(resource),
      headers_(detail::HttpResolvedPmrResourceTag{}, resource),
      body_(resource) {}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return body_.get_allocator().resource();
}

std::uint16_t HttpResponse::status() const noexcept {
    return statusCode_;
}

std::string_view HttpResponse::statusText() const noexcept {
    if (statusText_.empty()) {
        return detail::httpDefaultStatusText(statusCode_);
    }
    return statusText_;
}

const HttpResponseHeaders& HttpResponse::headers() const noexcept {
    return headers_;
}

std::string_view HttpResponse::bodyBytes() const noexcept {
    if (bodyKind_ == BodyKind::kOwned) {
        return std::string_view(body_.data(), body_.size());
    }
    if (bodyKind_ == BodyKind::kBorrowed ||
        bodyKind_ == BodyKind::kStaticBorrowed) {
        return bodyView_;
    }
    return {};
}

std::size_t HttpResponse::bodySize() const noexcept {
    if (bodyKind_ == BodyKind::kFile && fileBody_) {
        return static_cast<std::size_t>(fileBody_->length_);
    }
    return bodyBytes().size();
}

bool HttpResponse::hasFileBody() const noexcept {
    return bodyKind_ == BodyKind::kFile && fileBody_.has_value();
}

const HttpResponse::FileBody& HttpResponse::fileBody() const {
    if (!fileBody_) {
        throw std::logic_error("response does not contain a file body");
    }
    return *fileBody_;
}

void HttpResponse::setStreamBody(StreamBody body) noexcept {
    // A streaming body supersedes any buffered/file body already set.
    body_.clear();
    bodyView_ = {};
    fileBody_.reset();
    streamBody_ = std::move(body);
    bodyKind_ = BodyKind::kStream;
}

bool HttpResponse::hasStreamBody() const noexcept {
    return bodyKind_ == BodyKind::kStream && streamBody_.has_value();
}

HttpResponse::StreamBody& HttpResponse::streamBody() noexcept {
    return *streamBody_;
}

const HttpResponse::StreamBody& HttpResponse::streamBody() const noexcept {
    return *streamBody_;
}

void HttpResponse::status(std::uint16_t statusCode, std::string_view statusText) {
    if (statusCode < 100 || statusCode > 999) {
        throw std::invalid_argument("invalid HTTP status code");
    }
    statusCode_ = statusCode;
    if (statusText.empty()) {
        statusText_.clear();
        return;
    }
    if (!isValidHttpStatusText(statusText)) {
        throw std::invalid_argument("invalid HTTP status text");
    }
    if (statusText == detail::httpDefaultStatusText(statusCode)) {
        statusText_.clear();
        return;
    }
    statusText_.assign(statusText.data(), statusText.size());
}

void HttpResponse::setBodyCopy(std::string_view value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? BodyKind::kEmpty : BodyKind::kOwned;
    body_.assign(value.data(), value.size());
}

void HttpResponse::setBodyView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? BodyKind::kEmpty : BodyKind::kBorrowed;
}

void HttpResponse::setBodyStaticView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? BodyKind::kEmpty : BodyKind::kStaticBorrowed;
}

void HttpResponse::setBodyOwned(std::pmr::string&& value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? BodyKind::kEmpty : BodyKind::kOwned;
    body_ = std::move(value);
}

void HttpResponse::materializeBody() {
    if (bodyKind_ != BodyKind::kBorrowed) {
        return;
    }

    body_.assign(bodyView_.data(), bodyView_.size());
    bodyView_ = {};
    bodyKind_ = body_.empty() ? BodyKind::kEmpty : BodyKind::kOwned;
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

    body_.clear();
    bodyView_ = {};
    bodyKind_ = BodyKind::kFile;
    fileBody_.emplace(resource(), std::move(file), size, offset, length);
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

    body_.clear();
    bodyView_ = {};
    bodyKind_ = BodyKind::kFile;
    fileBody_.emplace(resource(), file.c_str(), size, offset, length);
}

void HttpResponse::setBorrowedNativeFileBody(const FileBody::NativePathChar* file, std::uint64_t size) {
    setBorrowedNativeFileBody(file, size, 0, size);
}

void HttpResponse::setBorrowedNativeFileBody(
    const FileBody::NativePathChar* file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    if (file == nullptr || *file == FileBody::NativePathChar{}) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.clear();
    bodyView_ = {};
    bodyKind_ = BodyKind::kFile;
    fileBody_.emplace(resource(), file, size, offset, length);
}

}  // namespace ruvia
