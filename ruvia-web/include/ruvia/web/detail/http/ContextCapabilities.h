#pragma once

#include <variant>

namespace ruvia {

class BodyReader;
class ResponseStreamWriter;
class WebSocket;

namespace detail {

class RequestBodyLoader;
class ContextRequestBodySource;
class ContextResponseOutput;

// A Context always has exactly one request-body source. The buffered alternative
// means the complete bytes are already exposed by HttpRequest; the other two
// borrow non-null runtime facades for the duration of dispatch.
class ContextBufferedRequestBodySource final {
private:
    friend class ContextRequestBodySource;
    constexpr ContextBufferedRequestBodySource() noexcept = default;
};

class ContextLazyRequestBodySource final {
public:
    [[nodiscard]] constexpr RequestBodyLoader& loader() const noexcept {
        return *loader_;
    }

private:
    friend class ContextRequestBodySource;

    explicit constexpr ContextLazyRequestBodySource(
        RequestBodyLoader& loader) noexcept
        : loader_(&loader) {}

    RequestBodyLoader* loader_;
};

class ContextStreamingRequestBodySource final {
public:
    [[nodiscard]] constexpr BodyReader& reader() const noexcept {
        return *reader_;
    }

private:
    friend class ContextRequestBodySource;

    explicit constexpr ContextStreamingRequestBodySource(
        BodyReader& reader) noexcept
        : reader_(&reader) {}

    BodyReader* reader_;
};

class ContextRequestBodySource final {
public:
    constexpr ContextRequestBodySource() noexcept
        : value_(ContextBufferedRequestBodySource{}) {}

    [[nodiscard]] static constexpr ContextRequestBodySource lazy(
        RequestBodyLoader& loader) noexcept {
        return ContextRequestBodySource(
            ContextLazyRequestBodySource(loader));
    }

    [[nodiscard]] static constexpr ContextRequestBodySource streaming(
        BodyReader& reader) noexcept {
        return ContextRequestBodySource(
            ContextStreamingRequestBodySource(reader));
    }

    [[nodiscard]] constexpr const ContextBufferedRequestBodySource* buffered()
        const & noexcept {
        return std::get_if<ContextBufferedRequestBodySource>(&value_);
    }
    [[nodiscard]] constexpr const ContextBufferedRequestBodySource* buffered()
        const && = delete;

    [[nodiscard]] constexpr const ContextLazyRequestBodySource* lazy()
        const & noexcept {
        return std::get_if<ContextLazyRequestBodySource>(&value_);
    }
    [[nodiscard]] constexpr const ContextLazyRequestBodySource* lazy()
        const && = delete;

    [[nodiscard]] constexpr const ContextStreamingRequestBodySource* streaming()
        const & noexcept {
        return std::get_if<ContextStreamingRequestBodySource>(&value_);
    }
    [[nodiscard]] constexpr const ContextStreamingRequestBodySource* streaming()
        const && = delete;

private:
    using Value = std::variant<
        ContextBufferedRequestBodySource,
        ContextLazyRequestBodySource,
        ContextStreamingRequestBodySource>;

    template <typename Source>
    explicit constexpr ContextRequestBodySource(Source source) noexcept
        : value_(source) {}

    Value value_;
};

// A Context likewise has exactly one response output. The buffered alternative
// uses the ordinary HttpResponse return path; long-lived outputs borrow one
// non-null runtime facade and cannot coexist in the same Context.
class ContextBufferedResponseOutput final {
private:
    friend class ContextResponseOutput;
    constexpr ContextBufferedResponseOutput() noexcept = default;
};

class ContextResponseStreamOutput final {
public:
    [[nodiscard]] constexpr ResponseStreamWriter& writer() const noexcept {
        return *writer_;
    }

private:
    friend class ContextResponseOutput;

    explicit constexpr ContextResponseStreamOutput(
        ResponseStreamWriter& writer) noexcept
        : writer_(&writer) {}

    ResponseStreamWriter* writer_;
};

class ContextWebSocketOutput final {
public:
    [[nodiscard]] constexpr WebSocket& webSocket() const noexcept {
        return *webSocket_;
    }

private:
    friend class ContextResponseOutput;

    explicit constexpr ContextWebSocketOutput(WebSocket& webSocket) noexcept
        : webSocket_(&webSocket) {}

    WebSocket* webSocket_;
};

class ContextResponseOutput final {
public:
    constexpr ContextResponseOutput() noexcept
        : value_(ContextBufferedResponseOutput{}) {}

    [[nodiscard]] static constexpr ContextResponseOutput responseStream(
        ResponseStreamWriter& writer) noexcept {
        return ContextResponseOutput(ContextResponseStreamOutput(writer));
    }

    [[nodiscard]] static constexpr ContextResponseOutput webSocket(
        WebSocket& webSocket) noexcept {
        return ContextResponseOutput(ContextWebSocketOutput(webSocket));
    }

    [[nodiscard]] constexpr const ContextBufferedResponseOutput* buffered()
        const & noexcept {
        return std::get_if<ContextBufferedResponseOutput>(&value_);
    }
    [[nodiscard]] constexpr const ContextBufferedResponseOutput* buffered()
        const && = delete;

    [[nodiscard]] constexpr const ContextResponseStreamOutput* responseStream()
        const & noexcept {
        return std::get_if<ContextResponseStreamOutput>(&value_);
    }
    [[nodiscard]] constexpr const ContextResponseStreamOutput* responseStream()
        const && = delete;

    [[nodiscard]] constexpr const ContextWebSocketOutput* webSocket()
        const & noexcept {
        return std::get_if<ContextWebSocketOutput>(&value_);
    }
    [[nodiscard]] constexpr const ContextWebSocketOutput* webSocket()
        const && = delete;

private:
    using Value = std::variant<
        ContextBufferedResponseOutput,
        ContextResponseStreamOutput,
        ContextWebSocketOutput>;

    template <typename Output>
    explicit constexpr ContextResponseOutput(Output output) noexcept
        : value_(output) {}

    Value value_;
};

}  // namespace detail
}  // namespace ruvia
