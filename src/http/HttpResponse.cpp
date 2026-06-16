#include "ruvia/http/HttpTypes.h"

#include "ruvia/http/HeaderUtils.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ruvia {

HttpResponseHeaders::HttpResponseHeaders(std::pmr::memory_resource* resource)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      heap_(resource_) {}

HttpResponseHeaders::~HttpResponseHeaders() {
    clear();
}

HttpResponseHeaders::HttpResponseHeaders(HttpResponseHeaders&& other) noexcept
    : resource_(other.resource_),
      heap_(resource_) {
    moveFrom(std::move(other));
}

HttpResponseHeaders& HttpResponseHeaders::operator=(HttpResponseHeaders&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    resource_ = other.resource_;
    heap_ = std::pmr::vector<HttpResponseHeader>(resource_);
    spilled_ = false;
    moveFrom(std::move(other));
    return *this;
}

void HttpResponseHeaders::reserve(std::size_t count) {
    if (count <= kInlineCapacity) {
        return;
    }

    spill();
    heap_.reserve(count);
}

HttpResponseHeader HttpResponseHeaders::makeHeader(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto total = name.size() + value.size();
    char* bytes = nullptr;
    if (total > 0) {
        bytes = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes, name.data(), name.size());
        std::memcpy(bytes + name.size(), value.data(), value.size());
    }
    return HttpResponseHeader{
        .bytes = bytes,
        .nameSize = static_cast<std::uint32_t>(name.size()),
        .valueSize = static_cast<std::uint32_t>(value.size()),
        .knownBit = knownBit};
}

void HttpResponseHeaders::releaseHeader(HttpResponseHeader& header) noexcept {
    if (header.bytes != nullptr) {
        resource_->deallocate(
            const_cast<char*>(header.bytes),
            static_cast<std::size_t>(header.nameSize) + header.valueSize,
            1);
        header.bytes = nullptr;
        header.nameSize = 0;
        header.valueSize = 0;
    }
}

HttpResponseHeader& HttpResponseHeaders::add(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    if (!spilled_ && size_ == kInlineCapacity) {
        spill();
    }
    const auto header = makeHeader(name, value, knownBit);
    if (!spilled_) {
        auto* target = inlineData() + size_;
        *target = header;
        ++size_;
        return *target;
    }
    heap_.push_back(header);
    return heap_.back();
}

void HttpResponseHeaders::assign(
    HttpResponseHeader& header,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    // Allocate the replacement first so a bad_alloc leaves the slot intact.
    const auto replacement = makeHeader(name, value, knownBit);
    releaseHeader(header);
    header = replacement;
}

HttpResponseHeader* HttpResponseHeaders::inlineData() noexcept {
    return reinterpret_cast<HttpResponseHeader*>(inline_.data());
}

const HttpResponseHeader* HttpResponseHeaders::inlineData() const noexcept {
    return reinterpret_cast<const HttpResponseHeader*>(inline_.data());
}

HttpResponseHeader* HttpResponseHeaders::data() noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

const HttpResponseHeader* HttpResponseHeaders::data() const noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

void HttpResponseHeaders::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        releaseHeader(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void HttpResponseHeaders::spill() {
    if (spilled_) {
        return;
    }

    heap_.reserve(std::max<std::size_t>(kInlineCapacity * 2, size_ + 1));
    auto* items = inlineData();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void HttpResponseHeaders::moveFrom(HttpResponseHeaders&& other) noexcept {
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    // Headers are trivially relocatable: a single memcpy transfers ownership
    // of every name/value allocation to this container.
    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(InlineStorage));
    }
    size_ = other.size_;
    other.size_ = 0;
}

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : statusText_("OK", resource),
      headers_(resource),
      body_(resource) {}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return body_.get_allocator().resource();
}

std::uint16_t HttpResponse::statusCode() const noexcept {
    return statusCode_;
}

std::string_view HttpResponse::statusText() const noexcept {
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
    if (!isValidHttpStatusText(statusText)) {
        throw std::invalid_argument("invalid HTTP status text");
    }
    statusCode_ = statusCode;
    statusText_.assign(statusText.data(), statusText.size());
}

std::uint32_t HttpResponse::classifyKnownHeader(std::string_view name) noexcept {
    switch (name.size()) {
        case 4:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Vary")) return kKnownHeaderVary;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Date")) return kKnownHeaderDate;
            if (detail::httpAsciiEqualsIgnoreCase(name, "ETag")) return kKnownHeaderEtag;
            return 0;
        case 5:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Allow")) return kKnownHeaderAllow;
            return 0;
        case 6:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Server")) return kKnownHeaderServer;
            return 0;
        case 8:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Location")) return kKnownHeaderLocation;
            return 0;
        case 10:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) return kKnownHeaderConnection;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Set-Cookie")) return kKnownHeaderSetCookie;
            return 0;
        case 12:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) return kKnownHeaderContentType;
            return 0;
        case 13:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Cache-Control")) return kKnownHeaderCacheControl;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept-Ranges")) return kKnownHeaderAcceptRanges;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Range")) return kKnownHeaderContentRange;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Last-Modified")) return kKnownHeaderLastModified;
            return 0;
        case 14:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) return kKnownHeaderContentLength;
            return 0;
        case 16:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) return kKnownHeaderContentEncoding;
            return 0;
        case 17:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) return kKnownHeaderTransferEncoding;
            return 0;
        case 27:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Origin")) return kKnownHeaderAccessControlAllowOrigin;
            return 0;
        case 28:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Methods")) return kKnownHeaderAccessControlAllowMethods;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Headers")) return kKnownHeaderAccessControlAllowHeaders;
            return 0;
        case 22:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Max-Age")) return kKnownHeaderAccessControlMaxAge;
            return 0;
        case 29:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Expose-Headers")) return kKnownHeaderAccessControlExposeHeaders;
            return 0;
        case 32:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Credentials")) return kKnownHeaderAccessControlAllowCredentials;
            return 0;
        default:
            return 0;
    }
}

std::size_t HttpResponse::knownHeaderSlot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t knownMask = (1U << kKnownHeaderCount) - 1U;
    if (bit == 0 || (bit & ~knownMask) != 0 || (bit & (bit - 1U)) != 0) {
        return kKnownHeaderCount;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

std::string_view HttpResponse::header(KnownHeaderBit bit) const noexcept {
    const auto slot = knownHeaderSlot(bit);
    if (slot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[slot];
        if (index >= 0) {
            return headers_.begin()[index].value();
        }
    }
    return {};
}

std::string_view HttpResponse::header(std::string_view name) const noexcept {
    if (const auto bit = classifyKnownHeader(name); bit != 0) {
        return header(static_cast<KnownHeaderBit>(bit));
    }

    for (const auto& header : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name)) {
            return header.value();
        }
    }
    return {};
}

void HttpResponse::setHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = classifyKnownHeader(key);
    const auto knownSlot = knownHeaderSlot(knownBit);
    if (knownSlot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[knownSlot];
        if (index >= 0) {
            headers_.assign(headers_.begin()[index], key, value, knownBit);
            return;
        }
        const auto nextIndex = headers_.size();
        headers_.add(key, value, knownBit);
        knownHeaderBits_ |= knownBit;
        knownHeaderIndexes_[knownSlot] = static_cast<std::int32_t>(nextIndex);
        return;
    }

    for (auto& header : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), key)) {
            headers_.assign(header, key, value, knownBit);
            return;
        }
    }

    appendHeader(key, value);
}

void HttpResponse::appendHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = classifyKnownHeader(key);
    const auto index = headers_.size();
    headers_.add(key, value, knownBit);
    if (knownBit != 0) {
        knownHeaderBits_ |= knownBit;
        const auto slot = knownHeaderSlot(knownBit);
        if (slot < knownHeaderIndexes_.size() && knownHeaderIndexes_[slot] < 0) {
            knownHeaderIndexes_[slot] = static_cast<std::int32_t>(index);
        }
    }
}

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

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
