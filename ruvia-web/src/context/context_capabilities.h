#pragma once

#include <variant>

namespace ruvia {

class body_reader;
class response_stream_writer;
class websocket;
class http_tunnel;

namespace detail {

class request_body_loader;
class context_request_body_source;
class context_response_output;

// A context always has exactly one request-body source. The buffered alternative
// means the complete bytes are already exposed by http_request; the other two
// borrow non-null runtime facades for the duration of dispatch.
class context_buffered_request_body_source final {
private:
    friend class context_request_body_source;
    constexpr context_buffered_request_body_source() noexcept = default;
};

class context_lazy_request_body_source final {
public:
    [[nodiscard]] constexpr request_body_loader& loader() const noexcept {
        return *loader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_lazy_request_body_source(request_body_loader& loader) noexcept
        : loader_(&loader) {}

    request_body_loader* loader_;
};

class context_streaming_request_body_source final {
public:
    [[nodiscard]] constexpr body_reader& reader() const noexcept {
        return *reader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_streaming_request_body_source(body_reader& reader_value) noexcept
        : reader_(&reader_value) {}

    body_reader* reader_;
};

class context_request_body_source final {
public:
    constexpr context_request_body_source() noexcept
        : value_(context_buffered_request_body_source{}) {}

    [[nodiscard]] static constexpr context_request_body_source lazy(
        request_body_loader& loader) noexcept {
        return context_request_body_source(context_lazy_request_body_source(loader));
    }

    [[nodiscard]] static constexpr context_request_body_source streaming(body_reader& reader_value) noexcept {
        return context_request_body_source(context_streaming_request_body_source(reader_value));
    }

    [[nodiscard]] constexpr const context_buffered_request_body_source* buffered() const& noexcept {
        return std::get_if<context_buffered_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_buffered_request_body_source* buffered() const&& = delete;

    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const& noexcept {
        return std::get_if<context_lazy_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const&& = delete;

    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const& noexcept {
        return std::get_if<context_streaming_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const&& = delete;

private:
    using value_type = std::variant<context_buffered_request_body_source, context_lazy_request_body_source,
        context_streaming_request_body_source>;

    template <typename source>
    explicit constexpr context_request_body_source(source source_value) noexcept
        : value_(source_value) {}

    value_type value_;
};

// A context likewise has exactly one response output. The buffered alternative
// uses the ordinary http_response return path; long-lived outputs borrow one
// non-null runtime facade and cannot coexist in the same context.
class context_buffered_response_output final {
private:
    friend class context_response_output;
    constexpr context_buffered_response_output() noexcept = default;
};

class context_response_stream_output final {
public:
    [[nodiscard]] constexpr response_stream_writer& writer() const noexcept {
        return *writer_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_response_stream_output(response_stream_writer& writer) noexcept
        : writer_(&writer) {}

    response_stream_writer* writer_;
};

class context_websocket_output final {
public:
    [[nodiscard]] constexpr websocket& get_websocket() const noexcept {
        return *websocket_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_websocket_output(websocket& websocket_value) noexcept
        : websocket_(&websocket_value) {}

    websocket* websocket_;
};

class context_tunnel_output final {
public:
    [[nodiscard]] constexpr http_tunnel& tunnel() const noexcept {
        return *tunnel_;
    }

private:
    friend class context_response_output;
    explicit constexpr context_tunnel_output(http_tunnel& tunnel) noexcept
        : tunnel_(&tunnel) {}
    http_tunnel* tunnel_;
};

class context_response_output final {
public:
    constexpr context_response_output() noexcept
        : value_(context_buffered_response_output{}) {}

    [[nodiscard]] static constexpr context_response_output response_stream(
        response_stream_writer& writer) noexcept {
        return context_response_output(context_response_stream_output(writer));
    }

    [[nodiscard]] static constexpr context_response_output websocket_value(websocket& websocket_value) noexcept {
        return context_response_output(context_websocket_output(websocket_value));
    }

    [[nodiscard]] static constexpr context_response_output tunnel(http_tunnel& tunnel) noexcept {
        return context_response_output(context_tunnel_output(tunnel));
    }
    [[nodiscard]] constexpr const context_tunnel_output* tunnel() const& noexcept {
        return std::get_if<context_tunnel_output>(&value_);
    }
    const context_tunnel_output* tunnel() const&& = delete;
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const& noexcept {
        return std::get_if<context_buffered_response_output>(&value_);
    }
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const&& = delete;

    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const& noexcept {
        return std::get_if<context_response_stream_output>(&value_);
    }
    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const&& = delete;

    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const& noexcept {
        return std::get_if<context_websocket_output>(&value_);
    }
    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const&& = delete;

private:
    using value_type = std::variant<context_buffered_response_output, context_response_stream_output,
        context_websocket_output, context_tunnel_output>;

    template <typename output_type>
    explicit constexpr context_response_output(output_type output) noexcept
        : value_(output) {}

    value_type value_;
};

}  // namespace detail
}  // namespace ruvia
