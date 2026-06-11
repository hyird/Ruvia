#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/FileResponse.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Model.h"
#include "ruvia/http/StaticFiles.h"
#include "ruvia/http/Validation.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif

namespace ruvia {

inline constexpr std::size_t kMaxRouteParams = 16;

#ifdef RUVIA_ENABLE_MARIADB
class DbHandle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif
namespace detail {
class DbRegistry;
class RedisRegistry;
}

struct RouteParamView {
    std::string_view name;
    std::string_view value;
};

struct CookieOptions {
    std::string_view path{"/"};
    std::string_view domain;
    std::string_view sameSite;
    std::int64_t maxAge{-1};
    bool httpOnly{false};
    bool secure{false};
};

class BodyReader final {
public:
    using Read = Task<std::optional<std::string_view>> (*)(void*);

    constexpr BodyReader(void* target, Read read) noexcept : target_(target), read_(read) {}

    BodyReader(const BodyReader&) = delete;
    BodyReader& operator=(const BodyReader&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return read_ != nullptr;
    }

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        if (read_ == nullptr) {
            throw std::logic_error("request body is not streamable");
        }
        return read_(target_);
    }

private:
    void* target_{nullptr};
    Read read_{nullptr};
};

class RequestBodyLoader final {
public:
    using ReadAll = Task<std::string_view> (*)(void*);
    using Discard = Task<void> (*)(void*);

    constexpr RequestBodyLoader(void* target, ReadAll readAll, Discard discard) noexcept
        : target_(target), readAll_(readAll), discard_(discard) {}

    RequestBodyLoader(const RequestBodyLoader&) = delete;
    RequestBodyLoader& operator=(const RequestBodyLoader&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return readAll_ != nullptr && discard_ != nullptr;
    }

    [[nodiscard]] Task<std::string_view> readAll() {
        if (readAll_ == nullptr) {
            throw std::logic_error("request body is not buffered");
        }
        return readAll_(target_);
    }

    Task<void> discard() {
        if (discard_ == nullptr) {
            throw std::logic_error("request body is not buffered");
        }
        return discard_(target_);
    }

private:
    void* target_{nullptr};
    ReadAll readAll_{nullptr};
    Discard discard_{nullptr};
};

class ResponseStreamWriter final {
public:
    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*);
    using BindContext = void (*)(void*, Context*) noexcept;
    using Scratch = std::pmr::string& (*)(void*) noexcept;

    constexpr ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        BindContext bindContext = nullptr,
        Scratch scratch = nullptr) noexcept
        : target_(target), write_(write), end_(end), bindContext_(bindContext), scratch_(scratch) {}

    ResponseStreamWriter(const ResponseStreamWriter&) = delete;
    ResponseStreamWriter& operator=(const ResponseStreamWriter&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return write_ != nullptr && end_ != nullptr;
    }

    Task<void> write(std::string_view chunk) {
        if (write_ == nullptr) {
            throw std::logic_error("response body is not streamable");
        }
        return write_(target_, chunk);
    }

    Task<void> end() {
        if (end_ == nullptr) {
            throw std::logic_error("response body is not streamable");
        }
        return end_(target_);
    }

    void bindContext(Context& context) noexcept {
        if (bindContext_ != nullptr) {
            bindContext_(target_, &context);
        }
    }

    [[nodiscard]] std::pmr::string& scratch() const {
        if (scratch_ != nullptr) {
            return scratch_(target_);
        }
        throw std::logic_error("response stream context is not bound");
    }

private:
    void* target_{nullptr};
    Write write_{nullptr};
    End end_{nullptr};
    BindContext bindContext_{nullptr};
    Scratch scratch_{nullptr};
};

struct SseMessage final {
    std::string_view data;
    std::string_view event;
    std::string_view id;
    std::optional<std::uint32_t> retry;
};

class SseWriter final {
public:
    SseWriter(ResponseStreamWriter& writer, std::pmr::memory_resource* resource) noexcept
        : writer_(writer) {
        (void)resource;
    }

    Task<void> writeSSE(const SseMessage& message) {
        auto& frame = writer_.scratch();
        if (!message.event.empty()) {
            frame.append("event: ");
            frame.append(message.event.data(), message.event.size());
            frame.push_back('\n');
        }
        if (!message.id.empty()) {
            frame.append("id: ");
            frame.append(message.id.data(), message.id.size());
            frame.push_back('\n');
        }
        if (message.retry.has_value()) {
            frame.append("retry: ");
            appendUnsigned(frame, *message.retry);
            frame.push_back('\n');
        }
        appendData(frame, message.data);
        frame.push_back('\n');
        co_await writer_.write(frame);
    }

    Task<void> end() {
        return writer_.end();
    }

private:
    static void appendData(std::pmr::string& frame, std::string_view data) {
        while (true) {
            const auto next = data.find('\n');
            const auto line = next == std::string_view::npos ? data : data.substr(0, next);
            frame.append("data: ");
            frame.append(line.data(), line.size());
            frame.push_back('\n');
            if (next == std::string_view::npos) {
                return;
            }
            data.remove_prefix(next + 1);
        }
    }

    static void appendUnsigned(std::pmr::string& frame, std::uint32_t value) {
        std::array<char, 10> buffer{};
        const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (ec != std::errc{}) {
            throw std::logic_error("failed to format SSE retry value");
        }
        frame.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }

    ResponseStreamWriter& writer_;
};

struct MultipartStreamPart {
    std::string_view name;
    std::string_view filename;
    std::string_view contentType;
    std::string_view body;
    bool partBegin{false};
    bool partEnd{false};
};

class MultipartReader final {
public:
    MultipartReader(BodyReader& bodyReader, std::string_view boundary, std::pmr::memory_resource* resource)
        : bodyReader_(bodyReader),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          buffer_(resource_),
          boundaryLine_(resource_),
          boundaryPrefix_(resource_),
          currentName_(resource_),
          currentFilename_(resource_),
          currentContentType_(resource_) {
        boundaryLine_.append("--");
        boundaryLine_.append(boundary.data(), boundary.size());
        boundaryPrefix_.append("\r\n");
        boundaryPrefix_.append(boundaryLine_);
    }

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> read() {
        for (;;) {
            compactPending();
            switch (state_) {
                case State::kBoundary:
                    co_await processBoundary();
                    if (state_ == State::kDone) {
                        co_return std::nullopt;
                    }
                    break;
                case State::kHeaders:
                    co_await processHeaders();
                    break;
                case State::kBody:
                    if (auto part = co_await readBodyChunk()) {
                        co_return part;
                    }
                    break;
                case State::kDone:
                    co_return std::nullopt;
            }
        }
    }

private:
    enum class State {
        kBoundary,
        kHeaders,
        kBody,
        kDone
    };

    static constexpr std::size_t kCompactConsumedPrefixBytes = 64 * 1024;

    [[nodiscard]] std::string_view bufferView() const noexcept {
        if (bufferOffset_ >= buffer_.size()) {
            return {};
        }
        return std::string_view(buffer_.data() + bufferOffset_, buffer_.size() - bufferOffset_);
    }

    void consume(std::size_t bytes) noexcept {
        const auto available = bufferView().size();
        bufferOffset_ += std::min(bytes, available);
        if (bufferOffset_ == buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
        }
    }

    void compactConsumedPrefix() {
        if (bufferOffset_ == 0) {
            return;
        }
        if (bufferOffset_ == buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
            return;
        }
        if (bufferOffset_ < kCompactConsumedPrefixBytes) {
            return;
        }
        buffer_.erase(0, bufferOffset_);
        bufferOffset_ = 0;
    }

    void compactPending() {
        if (pendingEraseBytes_ == 0) {
            return;
        }
        consume(pendingEraseBytes_);
        pendingEraseBytes_ = 0;
    }

    Task<bool> appendMore() {
        compactConsumedPrefix();
        auto chunk = co_await bodyReader_.read();
        if (!chunk) {
            co_return false;
        }
        buffer_.append(chunk->data(), chunk->size());
        co_return true;
    }

    Task<void> processBoundary() {
        for (;;) {
            if (bufferView().starts_with("\r\n")) {
                consume(2);
            }
            while (bufferView().size() < boundaryLine_.size() + 2) {
                if (!(co_await appendMore())) {
                    throw std::invalid_argument("invalid multipart body");
                }
            }
            const auto buffer = bufferView();
            if (!buffer.starts_with(std::string_view(boundaryLine_))) {
                throw std::invalid_argument("invalid multipart body");
            }
            const auto afterBoundary = boundaryLine_.size();
            if (buffer.substr(afterBoundary, 2) == "--") {
                state_ = State::kDone;
                co_return;
            }
            if (buffer.substr(afterBoundary, 2) == "\r\n") {
                consume(afterBoundary + 2);
                state_ = State::kHeaders;
                co_return;
            }
            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
        }
    }

    Task<void> processHeaders() {
        for (;;) {
            const auto buffer = bufferView();
            const auto headersEnd = buffer.find("\r\n\r\n");
            if (headersEnd == std::string_view::npos) {
                if (!(co_await appendMore())) {
                    throw std::invalid_argument("invalid multipart body");
                }
                continue;
            }

            const auto headers = buffer.substr(0, headersEnd);
            const auto disposition = detail::httpHeaderValueInBlock(headers, "Content-Disposition");
            if (!disposition || !detail::httpIsFormDataDisposition(*disposition)) {
                throw std::invalid_argument("invalid multipart content disposition");
            }
            const auto name = detail::httpDispositionParameter(*disposition, "name");
            if (!name) {
                throw std::invalid_argument("invalid multipart field name");
            }

            currentName_.assign(name->data(), name->size());
            currentFilename_.clear();
            currentContentType_.clear();
            if (const auto filename = detail::httpDispositionParameter(*disposition, "filename")) {
                currentFilename_.assign(filename->data(), filename->size());
            }
            if (const auto contentType = detail::httpHeaderValueInBlock(headers, "Content-Type")) {
                currentContentType_.assign(contentType->data(), contentType->size());
            }
            consume(headersEnd + 4);
            partBegin_ = true;
            state_ = State::kBody;
            co_return;
        }
    }

    [[nodiscard]] MultipartStreamPart makePart(std::string_view body, bool partEnd) {
        MultipartStreamPart part;
        part.name = currentName_;
        part.filename = currentFilename_;
        part.contentType = currentContentType_;
        part.body = body;
        part.partBegin = partBegin_;
        part.partEnd = partEnd;
        partBegin_ = false;
        return part;
    }

    Task<std::optional<MultipartStreamPart>> readBodyChunk() {
        for (;;) {
            const auto buffer = bufferView();
            const auto boundary = buffer.find(boundaryPrefix_);
            if (boundary != std::string_view::npos) {
                if (boundary > 0 || partBegin_) {
                    auto part = makePart(buffer.substr(0, boundary), true);
                    pendingEraseBytes_ = boundary;
                    state_ = State::kBoundary;
                    co_return part;
                }
                state_ = State::kBoundary;
                break;
            }

            const auto keepTail = boundaryLine_.size() + 6;
            if (buffer.size() > keepTail) {
                const auto bytes = buffer.size() - keepTail;
                auto part = makePart(buffer.substr(0, bytes), false);
                pendingEraseBytes_ = bytes;
                co_return part;
            }

            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
        }

        co_return std::nullopt;
    }

    BodyReader& bodyReader_;
    std::pmr::memory_resource* resource_;
    std::pmr::string buffer_;
    std::pmr::string boundaryLine_;
    std::pmr::string boundaryPrefix_;
    std::pmr::string currentName_;
    std::pmr::string currentFilename_;
    std::pmr::string currentContentType_;
    State state_{State::kBoundary};
    std::size_t bufferOffset_{0};
    std::size_t pendingEraseBytes_{0};
    bool partBegin_{false};
};

class Context final {
public:
    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        detail::DbRegistry* db = nullptr,
        detail::RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr,
        WebSocket* webSocket = nullptr) noexcept
        : memory_(memory),
          request_(request),
          db_(db),
          redis_(redis),
          bodyReader_(bodyReader),
          bodyLoader_(bodyLoader),
          webSocket_(webSocket),
          responseStatusText_("OK", memory.resource()),
          responseHeaders_(memory.resource()) {}

    ~Context() {
        clearValidatedValues();
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        const std::array<RouteParamView, kMaxRouteParams>& params,
        std::size_t paramCount,
        detail::DbRegistry* db = nullptr,
        detail::RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr,
        WebSocket* webSocket = nullptr) noexcept
        : memory_(memory),
          request_(request),
          params_(params.data()),
          paramCount_(std::min(paramCount, kMaxRouteParams)),
          db_(db),
          redis_(redis),
          bodyReader_(bodyReader),
          bodyLoader_(bodyLoader),
          webSocket_(webSocket),
          responseStatusText_("OK", memory.resource()),
          responseHeaders_(memory.resource()) {}

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        ResponseStreamWriter* responseStream,
        detail::DbRegistry* db = nullptr,
        detail::RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr,
        WebSocket* webSocket = nullptr) noexcept
        : Context(memory, request,
              db,
              redis,
              bodyReader,
              bodyLoader,
              webSocket) {
        responseStream_ = responseStream;
    }

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        const std::array<RouteParamView, kMaxRouteParams>& params,
        std::size_t paramCount,
        ResponseStreamWriter* responseStream,
        detail::DbRegistry* db = nullptr,
        detail::RedisRegistry* redis = nullptr,
        BodyReader* bodyReader = nullptr,
        RequestBodyLoader* bodyLoader = nullptr,
        WebSocket* webSocket = nullptr) noexcept
        : Context(memory, request, params, paramCount,
              db,
              redis,
              bodyReader,
              bodyLoader,
              webSocket) {
        responseStream_ = responseStream;
    }

    [[nodiscard]] const HttpRequest& req() const noexcept {
        return request_;
    }

    [[nodiscard]] std::optional<std::pmr::string> decodedPath() const {
        return request_.decodedPath();
    }

    [[nodiscard]] ParamValue param(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < paramCount_; ++i) {
            if (params_[i].name == name) {
                return ParamValue(params_[i].value, resource(), RequestValue::DecodeMode::kPercent);
            }
        }

        return ParamValue(std::nullopt, resource(), RequestValue::DecodeMode::kPercent);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        return request_.header(name);
    }

    [[nodiscard]] std::string_view header(HttpRequest::KnownHeader name) const noexcept {
        return request_.header(name);
    }

    [[nodiscard]] QueryValue query(std::string_view name) const noexcept {
        return request_.query(name);
    }

    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept {
        return request_.cookie(name);
    }

    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept {
        return detail::httpAcceptsMediaType(request_.header(HttpRequest::KnownHeader::kAccept), mediaType);
    }

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return request_.remoteAddress();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    [[nodiscard]] Task<std::string_view> body() const {
        if (bodyLoader_ != nullptr) {
            co_return co_await bodyLoader_->readAll();
        }
        if (bodyReader_ != nullptr) {
            throw std::logic_error("streaming request body cannot be buffered");
        }
        co_return request_.body_;
    }

    template <typename T>
    [[nodiscard]] Task<T> json() const {
        static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_MODEL");
        if (!detail::contentTypeMatches(request_.header(HttpRequest::KnownHeader::kContentType), "application/json")) {
            throw std::invalid_argument("invalid json content type");
        }
        const auto requestBody = co_await body();
        auto parsed = JsonBody<T>::parse(requestBody, resource());
        if (!parsed) {
            throw std::invalid_argument("invalid json body");
        }
        co_return std::move(*parsed);
    }

    template <typename T>
    [[nodiscard]] Task<T> form() const {
        static_assert(FormBody<T>::value, "form body type must use RUVIA_MODEL");
        if (!detail::contentTypeMatches(request_.header(HttpRequest::KnownHeader::kContentType), "application/x-www-form-urlencoded")) {
            throw std::invalid_argument("invalid form content type");
        }
        const auto requestBody = co_await body();
        auto parsed = FormBody<T>::parse(requestBody, resource());
        if (!parsed) {
            throw std::invalid_argument("invalid form body");
        }
        co_return std::move(*parsed);
    }

    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> multipart() const {
        const auto contentType = request_.header(HttpRequest::KnownHeader::kContentType);
        const auto mediaEnd = contentType.find(';');
        const auto mediaType = detail::httpTrimOws(
            mediaEnd == std::string_view::npos ? contentType : contentType.substr(0, mediaEnd));
        if (!detail::httpAsciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
            throw std::invalid_argument("invalid multipart content type");
        }

        const auto boundaryValue = detail::httpContentTypeParameter(contentType, "boundary");
        if (!boundaryValue || boundaryValue->empty()) {
            throw std::invalid_argument("invalid multipart boundary");
        }

        const auto requestBody = co_await body();
        std::pmr::string delimiter(resource());
        delimiter.append("--");
        delimiter.append(boundaryValue->data(), boundaryValue->size());
        std::pmr::string nextDelimiter(resource());
        nextDelimiter.append("\r\n");
        nextDelimiter.append(delimiter);

        std::pmr::vector<MultipartPart> parts(resource());
        auto cursor = requestBody.find(delimiter);
        if (cursor == std::string_view::npos) {
            throw std::invalid_argument("invalid multipart body");
        }

        cursor += delimiter.size();
        for (;;) {
            if (requestBody.substr(cursor, 2) == "--") {
                break;
            }
            if (requestBody.substr(cursor, 2) != "\r\n") {
                throw std::invalid_argument("invalid multipart body");
            }
            cursor += 2;

            const auto headersEnd = requestBody.find("\r\n\r\n", cursor);
            if (headersEnd == std::string_view::npos) {
                throw std::invalid_argument("invalid multipart body");
            }

            const auto headerBlock = requestBody.substr(cursor, headersEnd - cursor);
            const auto disposition = detail::httpHeaderValueInBlock(headerBlock, "Content-Disposition");
            if (!disposition || !detail::httpIsFormDataDisposition(*disposition)) {
                throw std::invalid_argument("invalid multipart content disposition");
            }

            const auto name = detail::httpDispositionParameter(*disposition, "name");
            if (!name) {
                throw std::invalid_argument("invalid multipart field name");
            }

            cursor = headersEnd + 4;
            const auto nextDelimiterPrefix = requestBody.find(std::string_view(nextDelimiter), cursor);
            if (nextDelimiterPrefix == std::string_view::npos) {
                throw std::invalid_argument("invalid multipart body");
            }

            MultipartPart part(resource());
            part.name.assign(name->data(), name->size());
            if (const auto filename = detail::httpDispositionParameter(*disposition, "filename")) {
                part.filename.assign(filename->data(), filename->size());
            }
            if (const auto partContentType = detail::httpHeaderValueInBlock(headerBlock, "Content-Type")) {
                part.contentType.assign(partContentType->data(), partContentType->size());
            }
            part.body = requestBody.substr(cursor, nextDelimiterPrefix - cursor);
            parts.emplace_back(std::move(part));

            cursor = nextDelimiterPrefix + 2 + delimiter.size();
        }

        co_return parts;
    }

    Task<void> discardBody() const {
        if (bodyLoader_ != nullptr) {
            co_await bodyLoader_->discard();
            co_return;
        }
        if (bodyReader_ != nullptr) {
            while (co_await bodyReader_->read()) {}
        }
    }

#ifdef RUVIA_ENABLE_MARIADB
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif

    [[nodiscard]] BodyReader& bodyReader() const {
        if (bodyReader_ == nullptr) {
            throw std::logic_error("request body is not streamable");
        }
        return *bodyReader_;
    }

    [[nodiscard]] MultipartReader multipartReader() const {
        const auto contentType = request_.header(HttpRequest::KnownHeader::kContentType);
        const auto mediaEnd = contentType.find(';');
        const auto mediaType = detail::httpTrimOws(
            mediaEnd == std::string_view::npos ? contentType : contentType.substr(0, mediaEnd));
        if (!detail::httpAsciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
            throw std::invalid_argument("invalid multipart content type");
        }
        const auto boundary = detail::httpContentTypeParameter(contentType, "boundary");
        if (!boundary || boundary->empty()) {
            throw std::invalid_argument("invalid multipart boundary");
        }
        return MultipartReader(bodyReader(), *boundary, resource());
    }

    [[nodiscard]] WebSocket& webSocket() const {
        if (webSocket_ == nullptr) {
            throw std::logic_error("websocket is not available");
        }
        return *webSocket_;
    }

    [[nodiscard]] ResponseStreamWriter& stream() const {
        if (responseStream_ == nullptr) {
            throw std::logic_error("response body is not streamable");
        }
        return *responseStream_;
    }

    [[nodiscard]] ResponseStreamWriter& streamText() {
        setHeader("Content-Type", "text/plain; charset=utf-8");
        return stream();
    }

    [[nodiscard]] SseWriter streamSSE() const {
        return SseWriter(stream(), resource());
    }

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    template <typename T>
    [[nodiscard]] const T& valid() const {
        return valid<T>(ValidationTarget::kJson);
    }

    template <typename T>
    [[nodiscard]] const T& valid(ValidationTarget target) const {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validationTypeKey<BodyT>();
        for (std::size_t i = 0; i < validatedValueCount_; ++i) {
            const auto& value = validatedValues_[i];
            if (value.target == target && value.typeKey == key) {
                return *static_cast<const BodyT*>(value.value);
            }
        }
        throw std::logic_error("validated request body is not available");
    }

    template <typename T>
    void setValid(ValidationTarget target, T&& body) {
        using BodyT = std::remove_cvref_t<T>;
        const auto* key = validationTypeKey<BodyT>();
        std::size_t slot = validatedValueCount_;
        for (std::size_t i = 0; i < validatedValueCount_; ++i) {
            const auto& value = validatedValues_[i];
            if (value.target == target && value.typeKey == key) {
                slot = i;
                break;
            }
        }

        if (slot == validatedValueCount_ && validatedValueCount_ == validatedValues_.size()) {
            throw std::logic_error("too many validated request bodies");
        }

        auto* stored = allocateValidatedBody<BodyT>(std::forward<T>(body));
        ValidatedValue next{
            target,
            key,
            stored,
            resource(),
            sizeof(BodyT),
            alignof(BodyT),
            &destroyValidatedBody<BodyT>};

        if (slot == validatedValueCount_) {
            validatedValues_[validatedValueCount_++] = next;
            return;
        }

        clearValidatedValue(validatedValues_[slot]);
        validatedValues_[slot] = next;
    }

    Context& status(std::uint16_t statusCode, std::string_view statusText = {}) {
        if (statusCode < 100 || statusCode > 999) {
            throw std::invalid_argument("invalid HTTP status code");
        }
        if (!isValidHttpStatusText(statusText)) {
            throw std::invalid_argument("invalid HTTP status text");
        }
        responseStatusCode_ = statusCode;
        const auto text = statusText.empty() ? defaultStatusText(statusCode) : statusText;
        responseStatusText_.assign(text.data(), text.size());
        return *this;
    }

    Context& setHeader(std::string_view name, std::string_view value) {
        if (!isValidHttpHeaderName(name)) {
            throw std::invalid_argument("invalid HTTP header name");
        }
        if (!isValidHttpHeaderValue(value)) {
            throw std::invalid_argument("invalid HTTP header value");
        }
        const auto knownBit = HttpResponse::classifyKnownHeader(name);
        for (auto& header : responseHeaders_) {
            if ((knownBit != 0 && header.knownBit == knownBit) ||
                (knownBit == 0 && detail::httpAsciiEqualsIgnoreCase(header.name(), name))) {
                responseHeaders_.assign(header, name, value, knownBit);
                return *this;
            }
        }

        responseHeaders_.add(name, value, knownBit);
        return *this;
    }

    Context& setCookie(std::string_view name, std::string_view value, const CookieOptions& options = {}) {
        validateCookie(name, value, options);
        std::pmr::string cookie(allocator<char>());
        cookie.append(name.data(), name.size());
        cookie.push_back('=');
        cookie.append(value.data(), value.size());
        if (!options.path.empty()) {
            cookie.append("; Path=");
            cookie.append(options.path.data(), options.path.size());
        }
        if (!options.domain.empty()) {
            cookie.append("; Domain=");
            cookie.append(options.domain.data(), options.domain.size());
        }
        if (options.maxAge >= 0) {
            std::array<char, 32> buffer{};
            const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), options.maxAge);
            if (ec == std::errc{}) {
                cookie.append("; Max-Age=");
                cookie.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
            }
        }
        if (options.httpOnly) {
            cookie.append("; HttpOnly");
        }
        if (options.secure) {
            cookie.append("; Secure");
        }
        if (!options.sameSite.empty()) {
            cookie.append("; SameSite=");
            cookie.append(options.sameSite.data(), options.sameSite.size());
        }

        responseHeaders_.add(
            "Set-Cookie",
            std::string_view(cookie.data(), cookie.size()),
            HttpResponse::kKnownHeaderSetCookie);
        return *this;
    }

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const {
        return textView(body, statusCode, statusText);
    }

    [[nodiscard]] HttpResponse textView(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const {
        HttpResponse response(resource());
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        response.setBodyView(body);
        applyResponseState(response, statusCode, statusText);
        return response;
    }

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const {
        HttpResponse response(resource());
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        response.setBody(std::move(body));
        applyResponseState(response, statusCode, statusText);
        return response;
    }

    [[nodiscard]] HttpResponse text(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const {
        HttpResponse response(resource());
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
        response.setBodyStaticView(std::string_view(body, size));
        applyResponseState(response, statusCode, statusText);
        return response;
    }

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const {
        HttpResponse response(resource());
        response.setHeader("Content-Type", "application/json; charset=utf-8");
        std::pmr::string body(allocator<char>());
        appendJson(body, value);
        response.setBody(std::move(body));
        applyResponseState(response, statusCode, statusText);
        return response;
    }

    [[nodiscard]] HttpResponse redirect(
        std::string_view location,
        std::uint16_t statusCode = 302,
        std::string_view statusText = {}) const {
        HttpResponse response(resource());
        response.setHeader("Location", location);
        applyResponseState(response, statusCode, statusText);
        return response;
    }

    [[nodiscard]] HttpResponse file(
        const std::filesystem::path& path,
        std::string_view contentType = {}) const {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            throw HttpError(404, "not_found", "file not found");
        }

        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            throw HttpError(404, "not_found", "file not found");
        }

        const auto modified = std::filesystem::last_write_time(path, ec);
        if (ec) {
            throw HttpError(404, "not_found", "file not found");
        }

        return fileWithMetadata(FileToken(path), path, static_cast<std::uint64_t>(size), modified, contentType);
    }

    [[nodiscard]] HttpResponse staticFile(
        const StaticRoot& root,
        std::string_view relativePath,
        std::string_view contentType = {}) const {
        auto relative = normalizeStaticRelativePath(relativePath, allocator<char>());

        if (relative.empty() && !root.hasDirectoryIndex()) {
            throw HttpError(403, "forbidden", "invalid static file path");
        }

        const auto* entry = root.find(relative);
        if (entry == nullptr && root.isIndexedDirectory(relative)) {
            if (!relative.empty() && relative.back() != '/') {
                relative.push_back('/');
            }
            relative.append(root.indexFile().data(), root.indexFile().size());
            entry = root.find(relative);
        }
        if (entry == nullptr) {
            throw HttpError(404, "not_found", "file not found");
        }

        return fileWithMetadata(
            FileToken::borrow(entry->file),
            detail::fileTokenPath(entry->file),
            entry->size,
            entry->modified,
            contentType.empty() ? std::string_view(entry->contentType) : contentType,
            entry->cacheControl,
            entry->enableRanges,
            entry->enableValidators,
            entry->etag,
            entry->lastModified);
    }

    [[nodiscard]] HttpResponse error(
        std::uint16_t statusCode,
        std::string_view code,
        std::string_view message,
        std::string_view statusText = {}) const {
        return makeErrorResponse(
            resource(),
            HttpErrorInfo{
                .statusCode = statusCode,
                .statusText = statusText,
                .code = code,
                .message = message});
    }

    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const {
        HttpResponse response(resource());
        if (!contentType.empty()) {
            response.setHeader("Content-Type", contentType);
        }
        applyResponseState(response, 0, {});
        return response;
    }

private:
    static void validateCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
        if (!isValidHttpHeaderName(name)) {
            throw std::invalid_argument("invalid cookie name");
        }
        if (!isValidCookieValue(value)) {
            throw std::invalid_argument("invalid cookie value");
        }
        if (!isValidCookieAttribute(options.path) ||
            !isValidCookieAttribute(options.domain) ||
            !isValidCookieAttribute(options.sameSite)) {
            throw std::invalid_argument("invalid cookie attribute");
        }
    }

    [[nodiscard]] static bool isValidCookieValue(std::string_view value) noexcept {
        for (const auto c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool isValidCookieAttribute(std::string_view value) noexcept {
        for (const auto c : value) {
            if (c == '\r' || c == '\n' || c == '\0' || c == ';') {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool etagListMatches(
        std::string_view values,
        std::string_view expected,
        bool strong) noexcept {
        while (!values.empty()) {
            const auto comma = values.find(',');
            const auto value = detail::httpTrimOws(
                comma == std::string_view::npos ? values : values.substr(0, comma));
            if (strong ? detail::httpStrongEtagEquals(value, expected) : detail::httpWeakEtagEquals(value, expected)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            values.remove_prefix(comma + 1);
        }
        return false;
    }

    [[nodiscard]] static bool ifMatchAllows(std::string_view header, std::string_view etag) noexcept {
        if (header.empty()) {
            return true;
        }
        if (detail::httpTrimOws(header) == "*") {
            return true;
        }
        return etagListMatches(header, etag, true);
    }

    [[nodiscard]] static bool ifNoneMatchMatches(std::string_view header, std::string_view etag) noexcept {
        if (header.empty()) {
            return false;
        }
        if (detail::httpTrimOws(header) == "*") {
            return true;
        }
        return etagListMatches(header, etag, false);
    }

    [[nodiscard]] static bool httpDateNotModified(std::string_view header, std::time_t modifiedSeconds) noexcept {
        const auto date = detail::httpParseImfFixdate(detail::httpTrimOws(header));
        return date.has_value() && modifiedSeconds <= *date;
    }

    [[nodiscard]] static bool httpDateUnmodified(std::string_view header, std::time_t modifiedSeconds) noexcept {
        const auto date = detail::httpParseImfFixdate(detail::httpTrimOws(header));
        return !date.has_value() || modifiedSeconds <= *date;
    }

    [[nodiscard]] static bool ifRangeAllows(
        std::string_view header,
        std::string_view etag,
        std::time_t modifiedSeconds) noexcept {
        if (header.empty()) {
            return true;
        }
        const auto value = detail::httpTrimOws(header);
        if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
            return detail::httpStrongEtagEquals(value, etag);
        }
        return httpDateNotModified(value, modifiedSeconds);
    }

    [[nodiscard]] static bool isWindowsDrivePath(std::string_view path) noexcept {
        if (path.size() < 2 || path[1] != ':') {
            return false;
        }
        const auto c = path.front();
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    [[nodiscard]] static std::pmr::string normalizeStaticRelativePath(
        std::string_view input,
        std::pmr::polymorphic_allocator<char> allocator) {
        if (!input.empty() && (input.front() == '/' || input.front() == '\\' || isWindowsDrivePath(input))) {
            throw HttpError(403, "forbidden", "invalid static file path");
        }

        std::pmr::string output(allocator);
        std::size_t cursor = 0;
        while (cursor <= input.size()) {
            const auto slash = input.find_first_of("/\\", cursor);
            const auto end = slash == std::string_view::npos ? input.size() : slash;
            const auto segment = input.substr(cursor, end - cursor);

            if (!segment.empty() && segment != ".") {
                if (segment == "..") {
                    if (output.empty()) {
                        throw HttpError(403, "forbidden", "invalid static file path");
                    }
                    const auto previousSlash = output.rfind('/');
                    if (previousSlash == std::pmr::string::npos) {
                        output.clear();
                    } else {
                        output.erase(previousSlash);
                    }
                } else {
                    if (!output.empty()) {
                        output.push_back('/');
                    }
                    output.append(segment.data(), segment.size());
                }
            }

            if (slash == std::string_view::npos) {
                break;
            }
            cursor = slash + 1;
        }

        return output;
    }

    struct FileConditionalHeaders final {
        std::string_view ifMatch;
        std::string_view ifUnmodifiedSince;
        std::string_view ifNoneMatch;
        std::string_view ifModifiedSince;
        std::string_view range;
        std::string_view ifRange;
    };

    [[nodiscard]] FileConditionalHeaders fileConditionalHeaders() const noexcept {
        return FileConditionalHeaders{
            request_.header(HttpRequest::KnownHeader::kIfMatch),
            request_.header(HttpRequest::KnownHeader::kIfUnmodifiedSince),
            request_.header(HttpRequest::KnownHeader::kIfNoneMatch),
            request_.header(HttpRequest::KnownHeader::kIfModifiedSince),
            request_.header(HttpRequest::KnownHeader::kRange),
            request_.header(HttpRequest::KnownHeader::kIfRange)};
    }

    [[nodiscard]] HttpResponse fileWithMetadata(
        FileToken file,
        const std::filesystem::path& path,
        std::uint64_t size,
        std::filesystem::file_time_type modified,
        std::string_view contentType,
        std::string_view cacheControl = {},
        bool enableRanges = true,
        bool enableValidators = true,
        std::string_view precomputedEtag = {},
        std::string_view precomputedLastModified = {}) const {
        std::pmr::string etagStorage(resource());
        std::pmr::string lastModifiedStorage(resource());
        std::string_view etag;
        std::string_view lastModified;
        const auto modifiedSeconds = detail::httpFileTimeToTimeT(modified);
        if (enableValidators) {
            if (precomputedEtag.empty() || precomputedLastModified.empty()) {
                etagStorage = detail::httpMakeFileEtag(resource(), size, modified);
                lastModifiedStorage = detail::httpFormatDate(resource(), modifiedSeconds);
                etag = etagStorage;
                lastModified = lastModifiedStorage;
            } else {
                etag = precomputedEtag;
                lastModified = precomputedLastModified;
            }
        }

        auto addFileHeaders = [&](HttpResponse& response) {
            response.reserveHeaders(kFileResponseHeaderReserve);
            response.setHeader("Content-Type", contentType.empty() ? detail::httpGuessContentType(path) : contentType);
            if (!cacheControl.empty()) {
                response.setHeader("Cache-Control", cacheControl);
            }
            if (enableRanges) {
                response.setHeader("Accept-Ranges", "bytes");
            }
            if (enableValidators) {
                response.setHeader("ETag", etag);
                response.setHeader("Last-Modified", lastModified);
            }
        };

        if (request_.method() == HttpMethod::kGet || request_.method() == HttpMethod::kHead) {
            const auto conditional = fileConditionalHeaders();
            if (enableValidators && !ifMatchAllows(conditional.ifMatch, etag)) {
                throw HttpError(412, "precondition_failed", "file precondition failed");
            }
            if (enableValidators && !conditional.ifUnmodifiedSince.empty() &&
                !httpDateUnmodified(conditional.ifUnmodifiedSince, modifiedSeconds)) {
                throw HttpError(412, "precondition_failed", "file precondition failed");
            }

            if (enableValidators && ifNoneMatchMatches(conditional.ifNoneMatch, etag)) {
                HttpResponse response(resource());
                addFileHeaders(response);
                applyResponseState(response, 304, {});
                return response;
            }

            if (enableValidators && conditional.ifNoneMatch.empty() &&
                !conditional.ifModifiedSince.empty() &&
                httpDateNotModified(conditional.ifModifiedSince, modifiedSeconds)) {
                HttpResponse response(resource());
                addFileHeaders(response);
                applyResponseState(response, 304, {});
                return response;
            }

            if (enableRanges && !conditional.range.empty()) {
                if (detail::httpByteRangeSetHasMultiple(conditional.range)) {
                    HttpResponse response(resource());
                    addFileHeaders(response);
                    response.setFileBody(std::move(file), size);
                    applyResponseState(response, 0, {});
                    return response;
                }

                if (enableValidators && !ifRangeAllows(conditional.ifRange, etag, modifiedSeconds)) {
                    HttpResponse response(resource());
                    addFileHeaders(response);
                    response.setFileBody(std::move(file), size);
                    applyResponseState(response, 0, {});
                    return response;
                }

                const auto parsedRange = detail::httpParseByteRange(conditional.range, size);
                if (!parsedRange) {
                    HttpResponse response(resource());
                    response.setHeader("Content-Range", detail::httpContentRangeUnsatisfied(resource(), size));
                    addFileHeaders(response);
                    applyResponseState(response, 416, {});
                    return response;
                }

                const auto [offset, length] = *parsedRange;
                HttpResponse response(resource());
                addFileHeaders(response);
                response.setHeader("Content-Range", detail::httpContentRange(resource(), offset, length, size));
                response.setFileBody(std::move(file), size, offset, length);
                applyResponseState(response, 206, {});
                return response;
            }
        }

        HttpResponse response(resource());
        addFileHeaders(response);
        response.setFileBody(std::move(file), size);
        applyResponseState(response, 0, {});
        return response;
    }
    void applyResponseState(
        HttpResponse& response,
        std::uint16_t statusCode,
        std::string_view statusText) const {
        const auto finalStatusCode = statusCode == 0 ? responseStatusCode_ : statusCode;
        const auto finalStatusText = statusText.empty()
            ? (statusCode == 0 ? std::string_view(responseStatusText_) : defaultStatusText(finalStatusCode))
            : statusText;

        response.setStatus(finalStatusCode, finalStatusText);
        for (const auto& header : responseHeaders_) {
            if (header.knownBit == HttpResponse::kKnownHeaderSetCookie) {
                response.appendHeader(header.name(), header.value());
            } else {
                response.setHeader(header.name(), header.value());
            }
        }
    }

    RequestMemory& memory_;
    const HttpRequest& request_;
    const RouteParamView* params_{nullptr};
    std::size_t paramCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    BodyReader* bodyReader_{nullptr};
    RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
    std::uint16_t responseStatusCode_{200};
    std::pmr::string responseStatusText_;
    HttpResponseHeaders responseHeaders_;

    struct ValidatedValue final {
        ValidationTarget target{ValidationTarget::kJson};
        const void* typeKey{nullptr};
        void* value{nullptr};
        std::pmr::memory_resource* resource{nullptr};
        std::size_t size{0};
        std::size_t alignment{0};
        void (*destroy)(void*) noexcept{nullptr};
    };

    template <typename T>
    [[nodiscard]] static const void* validationTypeKey() noexcept {
        static constexpr std::byte key{};
        return &key;
    }

    template <typename BodyT, typename ArgT>
    [[nodiscard]] BodyT* allocateValidatedBody(ArgT&& body) {
        auto* arena = resource();
        void* storage = arena->allocate(sizeof(BodyT), alignof(BodyT));
        try {
            return ::new (storage) BodyT(std::forward<ArgT>(body));
        } catch (...) {
            arena->deallocate(storage, sizeof(BodyT), alignof(BodyT));
            throw;
        }
    }

    template <typename T>
    static void destroyValidatedBody(void* value) noexcept {
        static_cast<T*>(value)->~T();
    }

    static void clearValidatedValue(ValidatedValue& value) noexcept {
        if (value.value != nullptr) {
            if (value.destroy != nullptr) {
                value.destroy(value.value);
            }
            if (value.resource != nullptr) {
                value.resource->deallocate(value.value, value.size, value.alignment);
            }
        }
        value = {};
    }

    void clearValidatedValues() noexcept {
        for (std::size_t i = 0; i < validatedValueCount_; ++i) {
            clearValidatedValue(validatedValues_[i]);
        }
        validatedValueCount_ = 0;
    }

    std::array<ValidatedValue, 8> validatedValues_{};
    std::size_t validatedValueCount_{0};

    static constexpr std::size_t kFileResponseHeaderReserve = 7;
};

}  // namespace ruvia
