#include "ruvia/http/HttpResponse.h"

#include "ruvia/http/HttpStatus.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : statusText_(resource),
      headers_(resource),
      body_(resource) {}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return body_.get_allocator().resource();
}

std::uint16_t HttpResponse::statusCode() const noexcept {
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

HttpResponseBodyKind HttpResponse::bodyKind() const noexcept {
    return bodyKind_;
}

std::string_view HttpResponse::bodyBytes() const noexcept {
    if (bodyKind_ == HttpResponseBodyKind::kOwned) {
        return std::string_view(body_.data(), body_.size());
    }
    if (bodyKind_ == HttpResponseBodyKind::kBorrowed ||
        bodyKind_ == HttpResponseBodyKind::kStaticBorrowed) {
        return bodyView_;
    }
    return {};
}

std::size_t HttpResponse::bodySize() const noexcept {
    if (bodyKind_ == HttpResponseBodyKind::kFile && fileBody_) {
        return static_cast<std::size_t>(fileBody_->length);
    }
    return bodyBytes().size();
}

bool HttpResponse::bodyEmpty() const noexcept {
    return bodySize() == 0;
}

bool HttpResponse::hasFileBody() const noexcept {
    return bodyKind_ == HttpResponseBodyKind::kFile && fileBody_.has_value();
}

const FileBody& HttpResponse::fileBody() const {
    if (!fileBody_) {
        throw std::logic_error("response does not contain a file body");
    }
    return *fileBody_;
}

void HttpResponse::setStatus(std::uint16_t statusCode, std::string_view statusText) {
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

namespace detail {

void setResponseBodyStaticView(HttpResponse& response, std::string_view value) noexcept {
    response.setBodyStaticView(value);
}

}  // namespace detail

void HttpResponse::setBodyCopy(std::string_view value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
    body_.assign(value.data(), value.size());
}

void HttpResponse::setBodyView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kBorrowed;
}

void HttpResponse::setBodyStaticView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kStaticBorrowed;
}

void HttpResponse::setBody(std::pmr::string&& value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
    body_ = std::move(value);
}

void HttpResponse::materializeBody() {
    if (bodyKind_ != HttpResponseBodyKind::kBorrowed) {
        return;
    }

    body_.assign(bodyView_.data(), bodyView_.size());
    bodyView_ = {};
    bodyKind_ = body_.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
}

void HttpResponse::setFileBody(FileToken file, std::uint64_t size) {
    setFileBody(std::move(file), size, 0, size);
}

void HttpResponse::setFileBody(FileToken file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.clear();
    bodyView_ = {};
    bodyKind_ = HttpResponseBodyKind::kFile;
    fileBody_ = FileBody{std::move(file), size, offset, length};
}

}  // namespace ruvia
