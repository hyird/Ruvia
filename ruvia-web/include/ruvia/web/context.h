#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/web/error.h"
#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"
#include "ruvia/web/web_worker.h"
namespace ruvia {

class context;
class env;
class static_root;
// Capabilities a handler reaches through context; include the defining header
// where they are used: http_client.h, websocket.h, http_tunnel.h,
// session.h, streaming.h and multipart_reader.h.
class http_client_ref;
class http_client_response;
class websocket;
class http_tunnel;
class test_app;
class session;
class body_reader;
class response_stream_writer;
class tls_client_hello_record;
class server_connection;
class sse_writer;
class multipart_reader;
class multipart_part;
#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif
namespace detail {
class context_request_storage;
class context_request_body_source;
class context_response_output;
class context_response_state;
class context_session_state;
template <typename t_type>
[[nodiscard]] request_binding_guard<t_type> bind_validated_model(context& context_value, const t_type& model);
template <typename t_type>
    requires(!std::is_lvalue_reference_v<t_type>)
request_binding_guard<std::remove_cvref_t<t_type>> bind_validated_model(context&, t_type&&) = delete;

[[noreturn]] void throw_invalid_query();
[[noreturn]] void throw_invalid_param();

struct context_access;
class context_services;
struct session_access;
}  // namespace detail

class plain_connection_transport final {
private:
    friend class conn_info;
    constexpr plain_connection_transport() noexcept = default;
};

class tls_connection_transport final {
public:
    // Verified client certificate subject DN. Absence means no certificate;
    // an engaged empty value is a certificate with an empty Subject (RFC 5280
    // section 4.1.2.6), not an unauthenticated connection.
    [[nodiscard]] constexpr std::optional<std::string_view> client_certificate_subject() const noexcept {
        return client_certificate_subject_;
    }

    // The application protocol the handshake selected (RFC 7301); empty when
    // the client offered no ALPN.
    [[nodiscard]] constexpr std::string_view alpn() const noexcept {
        return alpn_;
    }

    // The host_name the client sent in server_name (RFC 6066 §3); empty when
    // it sent none.
    [[nodiscard]] constexpr std::string_view server_name() const noexcept {
        return server_name_;
    }

private:
    friend class conn_info;
    friend class server_connection;

    constexpr tls_connection_transport(std::optional<std::string_view> client_certificate_subject,
        std::string_view alpn, std::string_view server_name) noexcept
        : client_certificate_subject_(client_certificate_subject),
          alpn_(alpn),
          server_name_(server_name) {}

    std::optional<std::string_view> client_certificate_subject_;
    std::string_view alpn_;
    std::string_view server_name_;
};

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside http_request and context_request.
class conn_info final {
public:
    // Process-wide unique transport ID across workers, hosts and restarts.
    // Every request and observation on one connection shares this ID; retired
    // IDs are never reused. Never zero for a served connection; zero for a
    // test_app request, which has no transport connection.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return id_;
    }

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use client() for the caller.
    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }
    // Who the request is from. Equal to remote() unless the peer is a configured
    // trusted proxy AND it sent a forwarding header, in which case it is what
    // that header names. Never derived from an untrusted peer's headers: those
    // are attacker-controlled, so with no trusted proxy configured this is
    // always the direct peer.
    //
    // This, not remote(), is what rate limiting keys on and what an access log
    // should record.
    [[nodiscard]] constexpr ip_endpoint client() const noexcept {
        return client_;
    }

    // Whether client()/scheme() came from a forwarding header rather than from
    // the transport. False for every request whose peer is not trusted.
    [[nodiscard]] constexpr bool via_trusted_proxy() const noexcept {
        return via_trusted_proxy_;
    }

    // https when the request reached the client over TLS -- including TLS the
    // proxy terminated and reported with X-Forwarded-Proto -- and http
    // otherwise. tls() describes only THIS hop, so a Secure cookie or an HSTS
    // header must be decided from this instead: behind a TLS-terminating proxy
    // the server's own transport is plaintext while the client's is not.
    [[nodiscard]] constexpr http_scheme scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] constexpr const plain_connection_transport* plain() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<plain_connection_transport>(&transport_);
    }
    const plain_connection_transport* plain() const&& = delete;

    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<tls_connection_transport>(&transport_);
    }
    const tls_connection_transport* tls() const&& = delete;

private:
    friend class detail::context_services;

    conn_info(std::uint64_t id, std::string_view remote_address, plain_connection_transport transport) noexcept
        : id_(id),
          remote_(ip_address::parse(remote_address).value_or(ip_address{}), 0),
          remote_text_(remote_address),
          client_(remote_),
          transport_(transport),
          scheme_(http_scheme::http) {}

    conn_info(std::uint64_t id, std::string_view remote_address, tls_connection_transport transport) noexcept
        : id_(id),
          remote_(ip_address::parse(remote_address).value_or(ip_address{}), 0),
          remote_text_(remote_address),
          client_(remote_),
          transport_(transport),
          scheme_(http_scheme::https) {}

    [[nodiscard]] static conn_info plain(std::uint64_t id, std::string_view remote_address) noexcept {
        return conn_info(id, remote_address, plain_connection_transport{});
    }

    [[nodiscard]] static conn_info tls(std::uint64_t id, std::string_view remote_address,
        std::optional<std::string_view> client_certificate_subject, std::string_view alpn,
        std::string_view server_name) noexcept {
        return conn_info(id, remote_address, tls_connection_transport(client_certificate_subject, alpn, server_name));
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty scheme leaves the transport-derived value.
    constexpr void apply_forwarded(
        const ip_address& client_address, std::optional<http_scheme> forwarded_scheme) noexcept {
        client_ = ip_endpoint(client_address, 0);
        via_trusted_proxy_ = true;
        if (forwarded_scheme.has_value()) {
            scheme_ = *forwarded_scheme;
        }
    }

    constexpr void set_remote_port(std::uint16_t port) noexcept {
        remote_ = ip_endpoint(remote_.address(), port);
        if (!via_trusted_proxy_) {
            client_ = remote_;
        }
    }
    void set_local(std::string_view address, std::uint16_t port) noexcept {
        local_ = ip_endpoint(ip_address::parse(address).value_or(ip_address{}), port);
    }

    std::uint64_t id_;
    ip_endpoint remote_;
    std::string_view remote_text_;
    ip_endpoint client_;
    ip_endpoint local_{};
    std::variant<plain_connection_transport, tls_connection_transport> transport_;
    http_scheme scheme_;
    bool via_trusted_proxy_{false};
};

static_assert(std::is_nothrow_copy_constructible_v<conn_info>);
static_assert(std::is_nothrow_move_constructible_v<conn_info>);
static_assert(std::is_nothrow_copy_assignable_v<conn_info>);
static_assert(std::is_nothrow_move_assignable_v<conn_info>);

// Inputs are copied before dispatch() returns its lazy scoped operation.
// Only origin-form targets and buffered request/response bodies are accepted.
// Cancellation combines `stop_token_` with the calling context's stop token;
// subrequest handlers have no total execution timeout.
struct dispatch_options final {
    borrowed_text method_{};
    borrowed_text target_{};
    borrowed_http_headers headers_{};
    borrowed_text body_{};
    stop_token stop_token_{};
};

// Owns its response metadata and body independently of the child subrequest.
class dispatch_response final {
public:
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

private:
    friend class context;
    friend class test_app;
    dispatch_response() = default;
    void copy_head_from(const http_response& response);
    http_status_code status_{http_status::ok};
    std::vector<http_header> headers_;
    std::string body_;
};

template <typename t_type>
using request_state_binding = detail::request_binding_guard<t_type>;

// Explicit opt-in for zero-copy response bodies: the response borrows these
// bytes instead of copying them. The consteval constructors accept only
// constant expressions that refer to static storage, such as string literals
// and namespace-scope or static constexpr arrays and views; frame-local or
// runtime buffers do not compile. Every other string handed to a context body
// builder is copied into response-owned storage.
class static_text final {
public:
    // Keeps embedded NUL bytes and drops only the literal's terminator.
    template <std::size_t n>
    consteval explicit static_text(const char (&literal)[n]) noexcept
        : value_(literal, n > 0 && literal[n - 1] == '\0' ? n - 1 : n) {}

    consteval explicit static_text(std::string_view value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return value_;
    }

private:
    std::string_view value_;
};

struct signed_cookie_lookup_options final {
    borrowed_text name_{};
    borrowed_text secret_{};
};

// Borrow-only request facade. Returned fields and body views belong to context,
// not this by-value facade. Model validation belongs to route middleware.
class context_request final {
public:
    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] http_known_method known_method() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view scheme() const noexcept;
    [[nodiscard]] std::string_view authority() const noexcept;
    [[nodiscard]] std::string_view target() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] http_request_target_form target_form() const noexcept;
    [[nodiscard]] std::string_view route_path() const noexcept;
    // Initial RFC 9218 Priority fields.
    [[nodiscard]] http_priority priority() const noexcept;

    // Accept uses media ranges; language uses RFC 4647 basic filtering;
    // charset matches tokens or '*'; encoding also handles coding aliases and
    // the default acceptability of identity.
    enum class negotiable_field : std::uint8_t { media_type,
        language,
        encoding,
        charset };
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    // Read after body completion to observe the complete terminal section.
    // Trailers never replace initial headers or change route selection.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;
    // Client weights select a caller-owned supported value; server order breaks
    // ties. An absent field selects the first offer, no acceptable offer is nullopt.
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_field field, std::span<const std::string_view> supported) const noexcept;
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_field field, std::initializer_list<std::string_view> supported) const noexcept {
        return negotiate(field, std::span<const std::string_view>(supported.begin(), supported.size()));
    }
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

    template <detail::parsed_integer t_type>
    [[nodiscard]] std::optional<t_type> query(std::string_view name) const;
    template <std::same_as<bool> t_type>
    [[nodiscard]] std::optional<bool> query(std::string_view name) const;
    template <detail::parsed_integer t_type>
    [[nodiscard]] std::optional<t_type> param(std::string_view name) const;
    template <std::same_as<bool> t_type>
    [[nodiscard]] std::optional<bool> param(std::string_view name) const;
    // Materialized once in request order, preserving duplicates. Header names
    // retain their semantic spelling; header get/count
    // and header_model binding compare names ASCII case-insensitively.
    [[nodiscard]] const request_name_value_list& header_fields() const;
    [[nodiscard]] const request_name_value_list& query_fields() const;
    [[nodiscard]] const request_name_value_list& cookie_fields() const;
    [[nodiscard]] const request_name_value_list& param_fields() const;
    [[nodiscard]] std::optional<std::string_view> signed_cookie(signed_cookie_lookup_options options) const;

    // Buffered views borrow context storage; read the media type with header("Content-Type").
    // `operation` bounds the wait for body bytes in each of these reads, and
    // in json_if(), form_if() and multipart(): a timeout or stop ends only
    // that call with std::system_error (timed_out or operation_canceled),
    // keeps what arrived, and a later call resumes; a stop requested before
    // the call starts throws operation_canceled. A non-positive timeout throws
    // std::invalid_argument. Server body timeouts still apply. This view is a
    // copyable handle: the operations borrow the context, not the view.
    [[nodiscard]] task<std::string_view> text(operation_options operation = {}) const;
    [[nodiscard]] task<std::span<const std::byte>> bytes(operation_options operation = {}) const;
    [[nodiscard]] task<void> discard_body(operation_options operation = {}) const;

    // Typed optional parsing with schema and field-rule validation. Only a
    // media-type mismatch yields nullopt; selected but malformed input throws
    // HTTP 400, and rule violations throw an http_error carrying the issues.
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> json_if(operation_options operation = {}) const;
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> form_if(operation_options operation = {}) const;
    template <typename t_type>
    [[nodiscard]] const t_type& validated() const;

    // Multipart remains a flat protocol representation, without dotted paths
    // or implicit array/group interpretation. Streaming is explicit-route only.
    // multipart() decodes a supported Content-Encoding; multipart_reader()
    // decodes none, so a coded body is refused with 415 and an invalid
    // Content-Encoding field with 400.
    [[nodiscard]] task<std::vector<multipart_part>> multipart(operation_options operation = {}) const;
    [[nodiscard]] ::ruvia::body_reader& body_reader() const;
    [[nodiscard]] ::ruvia::multipart_reader multipart_reader() const;

private:
    friend class context;
    explicit constexpr context_request(const context& context_value RUVIA_LIFETIMEBOUND) noexcept
        : context_(&context_value) {}

    template <typename t_type>
    [[nodiscard]] static std::optional<t_type> parse_typed(
        std::optional<std::string_view> raw, void (*fail)());

    const context* context_{nullptr};
};
struct redirect_response_options final {
    // URI reference: preserves existing %HH escapes and the first '#' separator;
    // subsequent '#' bytes are encoded as %23. CR and LF are rejected, as is a
    // '\' inside the authority of an absolute or "//" reference (browsers read
    // it as a path separator, so encoding it would change the target host).
    borrowed_text location_{};
    http_status_code status_{http_status::found};
};

struct set_cookie_options final {
    borrowed_text name_{};
    borrowed_text value_{};
    cookie_options attributes_{};
};

struct set_signed_cookie_options final {
    borrowed_text name_{};
    borrowed_text value_{};
    borrowed_text secret_{};
    cookie_options attributes_{};
};

struct delete_cookie_options final {
    borrowed_text name_{};
    cookie_options attributes_{};
};

// 显式选择草稿或已选响应的元数据编辑能力；不暴露可绕过提交检查的引用。
// 每次修改都检查尚未提交，保存 editor 也不能在提交后修改头。
class context_response_editor final {
public:
    void status(http_status_code status_code);
    void header(std::string_view name, std::string_view value, http_response_header_options options = {});
    void priority(http_priority_fields fields);
    void remove_header(std::string_view name);
    void set_cookie(set_cookie_options options);
    void set_signed_cookie(set_signed_cookie_options options);
    void delete_cookie(delete_cookie_options options);

private:
    friend class context;
    enum class target_type { draft,
        selected,
        policy };
    context_response_editor(context& owner, target_type target) noexcept
        : owner_(&owner),
          target_(target) {}
    [[nodiscard]] http_response& target() const;
    context* owner_;
    target_type target_;
};

// Read only by the static_file() call: the relative path is resolved and the
// content type copied into request storage before the task is returned.
struct static_file_response_options final {
    borrowed_text relative_path_{};
    borrowed_text content_type_{};
};

// The file context::file() serves, read only by that call: UTF-8 text, or a
// std::filesystem::path lvalue whose native string is used as is. Neither form
// builds a std::filesystem::path per request; file() copies the native form
// once before returning its task, NUL-terminating text (and widening it to
// UTF-16 on Windows). Like borrowed_text, it refuses owning temporaries, so a
// path assembled per request has to be named by the caller -- hoist it out of
// the handler when it is fixed.
class borrowed_file_path final {
public:
    constexpr borrowed_file_path() noexcept = default;

    template <typename text_type>
        requires std::constructible_from<borrowed_text, text_type&&>
    constexpr borrowed_file_path(text_type&& utf8) noexcept(std::is_nothrow_constructible_v<borrowed_text, text_type&&>)
        : text_(std::forward<text_type>(utf8)) {}

    borrowed_file_path(const std::filesystem::path& path) noexcept
        : native_(path.native()) {}

    borrowed_file_path(std::filesystem::path&&) = delete;
    borrowed_file_path(const std::filesystem::path&&) = delete;

    // The UTF-8 text form; empty when a std::filesystem::path is borrowed.
    [[nodiscard]] constexpr std::string_view text() const noexcept {
        return text_.view();
    }

    // A borrowed std::filesystem::path's NUL-terminated native string; empty
    // for the text form.
    [[nodiscard]] constexpr http_native_path_view_type native() const noexcept {
        return native_;
    }

private:
    borrowed_text text_{};
    http_native_path_view_type native_{};
};

// Read only by the file() call: the path and content type are copied before
// the task is returned.
struct file_response_options final {
    borrowed_file_path path_{};
    borrowed_text content_type_{};
};

class context final {
private:
    friend class context_request;
    friend class context_response_editor;
    friend class http_client_response;
    friend struct detail::context_access;
    friend struct detail::session_access;
    template <typename t_type>
    friend detail::request_binding_guard<t_type> detail::bind_validated_model(
        context& context_value, const t_type& model);

    context(const http_request& request, const detail::context_services& services);

    context(const http_request& request, std::string_view route_path,
        const std::string_view* param_names, const std::string_view* param_values,
        std::size_t param_count, std::uintptr_t route_rate_limit_scope,
        const detail::context_services& services);

public:
    // Sends a bodyless 1xx head before the final response. Fields are copied
    // before return; the operation and its storage retire on the owning worker.
    // Output is linear when started. Unstarted operations may coexist and do not
    // block the final response; starting one after the final head commits throws.
    // A failed write throws std::system_error, which is not routed through
    // on_error: the connection ends. A timeout or stop in `operation` resets
    // the request (HTTP/2, HTTP/3: the stream; HTTP/1: the connection) and
    // throws std::system_error (timed_out or operation_canceled); a stop
    // requested before the operation starts sends nothing.
    [[nodiscard]] task<void> inform(const http_interim_response_head& response, operation_options operation = {}) &;
    task<void> inform(const http_interim_response_head&, operation_options = {}) && = delete;

    // Queues a promised request on this HTTP/2 connection. False means the
    // peer disabled push or its stream capacity cannot admit the promise.
    [[nodiscard]] bool push(http_push_request_view request);
    // Connection-scoped ORIGIN (HTTP/2 and HTTP/3, requires TLS) and ALTSVC
    // (HTTP/2) advertisements.
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

    ~context();

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    [[nodiscard]] context_request req() const& noexcept RUVIA_LIFETIMEBOUND {
        return context_request(*this);
    }
    context_request req() const&& = delete;

    // Connection metadata borrows this request's transport state.
    [[nodiscard]] const conn_info& conn() const& noexcept RUVIA_LIFETIMEBOUND;
    const conn_info& conn() const&& = delete;
    // Copies shared ownership of the state returned by on_connection_open.
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept;
    // Closes the entire transport immediately, including every HTTP/2 or
    // HTTP/3 stream. A virtual test connection has no transport to close.
    void abort_connection() noexcept;

    // The exception that failed the current middleware/handler dispatch, or
    // null. Distinct from error(status, code, message) which constructs an
    // error response.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return error_;
    }

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return capabilities_.worker();
    }
    const worker_ref& worker() const&& = delete;

    // Borrows this worker's registered instance; never hand it to another
    // worker. An unregistered type throws std::logic_error.
    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable executes on the blocking pool and must own everything it
    // touches. Completion resumes on this worker; options.timeout_ or shutdown
    // stops the wait without joining a still-running foreign callable, and
    // options.stop_token_ is combined with stop_token().
    // Rejection and callable exceptions are retained in blocking_result. Inspect
    // status() for load shedding or use std::move(result).take_value() to throw.
    // With application::blocking_pool(nullptr) the result reports
    // blocking_status::unavailable without submitting the callable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options = {}) const {
        return capabilities_.run_blocking(std::move(callable), std::move(options));
    }

    // Observes worker shutdown (application::stop() closes every connection
    // at once, with no drain phase) and, for a subrequest, explicit
    // cancellation. db(), redis(), and run_blocking() bind this token
    // automatically. Explicit operation tokens are combined with it.
    //
    // Cancellation is cooperative: framework waits, including streaming sleep(),
    // observe this token; CPU work and raw Asio waits without it are not preempted.
    // Connection phase timeouts close the socket independently.
    //
    // 流式响应（stream()/stream_sse()）的 token 还观察客户端离开：HTTP/1 请求体
    // 完整消费后对端 FIN/RST 或写失败，HTTP/2 本流 RST_STREAM 或连接终止。
    // 缓冲响应路由不监测读侧终止。
    //
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept {
        return capabilities_.stop_token();
    }

    // Present only while session_middleware is bound for this request.
    [[nodiscard]] ruvia::session session();

    // Request-scoped typed state: how a middleware hands a value it computed --
    // an authenticated user, a resolved tenant, a trace span -- to everything it
    // calls through next(). The counterpart of worker_state<T>(), which lives for
    // the worker's whole life and is shared by every request on it; this lives
    // for one dynamic next() scope and is private to that request.
    //
    //   task<void> invoke(context& c, next& next_value) {
    //       const auto binding = c.bind_request_state(user);
    //       co_await next_value();                      // handler sees it
    //   }                                               // unbound here
    //
    // The binding stores the value by address and never copies it, so the bound
    // object must outlive the returned handle -- keep both in the middleware's
    // coroutine frame, as above. Binding a temporary is rejected at compile
    // time. One type is one slot: binding T again inside a nested scope shadows
    // the outer binding until the inner handle dies.
    //
    // Deliberately disjoint from req().validated<T>(): that answers "a validator
    // produced and checked this", and hand-bound state must never be able to
    // impersonate it.
    template <typename t_type>
    [[nodiscard]] request_state_binding<t_type> bind_request_state(const t_type& value);

    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    [[nodiscard]] request_state_binding<std::remove_cvref_t<t_type>> bind_request_state(t_type&&) = delete;

    // Throws std::logic_error when nothing of this type is bound.
    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& request_state() const;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_request_state() const noexcept {
        return request_bindings().try_get_state<t_type>();
    }
    [[nodiscard]] const ruvia::env& env() const noexcept;

    // Re-enter the immutable route table on this worker. Authentication,
    // validation and middleware run in a fresh request context.
    [[nodiscard]] task<dispatch_response> dispatch(dispatch_options options);
    [[nodiscard]] bool is_subrequest() const noexcept;

    // Builds a request path from a registered route pattern; the pattern is
    // the route's identity: c.url_for("/users/:id", {"42"}) -> "/users/42".
    // ":name" values are percent-encoded path segments and must be non-empty;
    // the value for a trailing "*" keeps its slashes and may be empty. The
    // result never starts with "//" and never carries a "." or ".." segment.
    // Throws std::invalid_argument for an unregistered pattern, a value-count
    // mismatch, a "."/".." segment, a pattern starting with an empty segment,
    // or a "*" value the route cannot capture back (one holding '\', or
    // starting with '/' at the root), and std::logic_error when the context
    // carries no route table (for example a hand-built test context).
    [[nodiscard]] std::string url_for(
        std::string_view pattern, std::initializer_list<std::string_view> values = {}) const;

    [[nodiscard]] std::string url_for(
        std::string_view pattern, std::span<const std::string_view> values) const;
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db() const;
    [[nodiscard]] db_session db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis() const;
    [[nodiscard]] redis_session redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_ref client() const;
    [[nodiscard]] http_client_ref client(std::string_view alias) const;
    [[nodiscard]] bool early_data() const noexcept;
    [[nodiscard]] ::ruvia::websocket& websocket() const;
    [[nodiscard]] http_tunnel& tunnel() const;

    [[nodiscard]] response_stream_writer& stream();

    // Sends text/plain; charset=UTF-8 and X-Content-Type-Options: nosniff,
    // like text(); an X-Content-Type-Options the application set is kept.
    [[nodiscard]] response_stream_writer& stream_text();

    [[nodiscard]] sse_writer stream_sse();

    // draft() 只编辑下一次构造的响应草稿，与是否调用 next()/respond() 无关。
    // body/text/json/html/file/static_file/redirect 成功后消费草稿，失败则保留；
    // stream commit 消费草稿。error() 独立构造，不继承草稿或已选响应。
    [[nodiscard]] context_response_editor draft() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::draft);
    }

    // 明确后处理已 respond 的缓冲响应；尚无响应或已提交时修改抛 logic_error。
    // 字段只属于当前响应，下一次替换不继承；跨替换字段显式写入 policy()。
    [[nodiscard]] context_response_editor edit_response() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::selected);
    }

    // 显式跨替换策略，只在最终交付/stream commit 时应用一次；builder 不继承它。
    // header/cookie 覆盖同名或同 cookie 身份的响应字段；append 保留重复语义。
    // 警告：显式 policy().status() 也覆盖错误响应；默认不强制任何状态。
    // response() 观察的是尚未应用 policy 的选定响应，不是最终 wire head。
    // 与 draft/edit_response 一样，提交后任何修改均抛 logic_error。
    [[nodiscard]] context_response_editor policy() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::policy);
    }

    // 只读观察已选择的缓冲响应（不包含尚待应用的 policy）；没有响应则为 nullptr。
    [[nodiscard]] const http_response* response() const noexcept;

    [[nodiscard]] bool committed() const noexcept;
    // 将独立响应移入 context；替换丢弃旧响应全部字段，不自动合并草稿。
    // next() 在 respond() 后启动是控制流错误；已提交后不得替换。
    void respond(http_response&& response);

    // Body builders copy their input into response-owned storage, except
    // static_text, which explicitly borrows static bytes without copying, and
    // std::string&&, which transfers existing string allocations.
    [[nodiscard]] http_response body(std::string_view body = {});
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response body(string_type&& body_value) {
        return body_owned(std::move(body_value));
    }
    [[nodiscard]] http_response body(std::span<const std::byte> body);
    [[nodiscard]] http_response body(static_text body);

    // text/plain; charset=UTF-8 with X-Content-Type-Options: nosniff (an
    // application-set X-Content-Type-Options is kept), like stream_text().
    [[nodiscard]] http_response text(std::string_view body);
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response text(string_type&& body_value) {
        return text_owned(std::move(body_value));
    }
    [[nodiscard]] http_response text(static_text body);

    template <typename t_type>
        requires detail::is_model_json_value<t_type>
    [[nodiscard]] http_response json(const t_type& value);

    [[nodiscard]] http_response html(std::string_view body);
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response html(string_type&& body_value) {
        return html_owned(std::move(body_value));
    }
    [[nodiscard]] http_response html(static_text body);
    [[nodiscard]] http_response redirect(redirect_response_options options);

    // File helpers honor If-Range only for matching strong ETags. Last-Modified
    // dates are weak and do not authorize a partial response.
    //
    // file() and static_file() open the file on this worker's blocking pool,
    // never on the worker, and complete with the settled response: status,
    // validators, Content-Length or Content-Range, and the opened handle the
    // response owns. The pool wait is bounded by server_config::write_timeout_
    // and stop_token(); a wait that cannot complete (no blocking pool, a full
    // queue, the timeout) throws a 503 file_read_unavailable http_error, and
    // a missing pool or a timed-out open is reported to the connection
    // failure channel.
    //
    // Both own their options before they return: the path, relative path and
    // content type are copied (and a static path resolved, its representation
    // selected) by the call, so a stored task never reads the caller's
    // storage. A path that cannot name a file (404), a rejected relative path
    // (403), an unindexed name (404) or an unacceptable coding (406) throws at
    // the call. Only static_file()'s `root` stays borrowed: it must outlive
    // the returned task.
    //
    // file() opens the file once and the response owns that handle; plain-TCP
    // transfers send from it without resolving the path again. The handle is
    // checked again right before sending: a file changed in place since file()
    // fails a single plain-TCP body before its head leaves and aborts any
    // other transfer before its bytes.
    [[nodiscard]] task<http_response> file(file_response_options options);

    // relative_path_ is a decoded file path below the root, such as
    // req().param("*") of a wildcard route; it is never percent-decoded again.
    // '/' and '\\' separate segments. A "." or ".." segment, an empty interior
    // segment, an absolute path, or a NUL is 403 -- rejected, never resolved,
    // whichever parameter it came from -- and an unindexed name 404. A file
    // that is no longer the indexed one is 500 static_file_changed.
    [[nodiscard]] task<http_response> static_file(
        const static_root& root, static_file_response_options options);
    // A temporary root would be destroyed before the returned task reads it.
    task<http_response> static_file(const static_root&&, static_file_response_options) = delete;

    [[nodiscard]] http_response error(http_error_info_options options) const;
    [[nodiscard]] http_response error(const http_error_info& info) const;

    [[nodiscard]] task<http_response> not_found();

private:
    [[nodiscard]] http_response streaming_head();
    [[nodiscard]] http_response body_owned(std::string&& body);
    [[nodiscard]] http_response text_owned(std::string&& body);
    [[nodiscard]] http_response html_owned(std::string&& body);

    [[nodiscard]] task<http_response> not_found_task();
    [[nodiscard]] task<dispatch_response> dispatch_task(std::string method,
        std::string target, std::string body, std::vector<http_header> headers,
        ::ruvia::stop_token stop);
    context& set_stable_response_header(std::string_view name, std::string_view value);
    void apply_response_state(http_response& response, std::optional<http_status_code> status_code);

    [[nodiscard]] http_response json_serialized(std::string& body);

    [[nodiscard]] std::string& decoded_body() const;
    [[nodiscard]] detail::context_request_storage& request_storage() const;
    [[nodiscard]] detail::context_services& services() noexcept;
    [[nodiscard]] const detail::context_services& services() const noexcept;
    void store_response(http_response&& response);
    void store_error(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool has_response() const noexcept;

    const http_request& request_;
    // context cannot escape request dispatch and therefore borrows the stable
    // server-owned handle without touching its shared ownership count.
    detail::worker_context_capabilities capabilities_;
    std::string_view route_path_;
    const std::string_view* param_names_{nullptr};
    const std::string_view* param_values_{nullptr};
    std::size_t param_count_{0};
    std::uintptr_t route_rate_limit_scope_{0};
    using request_storage_owner_type = std::unique_ptr<detail::context_request_storage>;
    // One typed allocation owns request caches, response/session state,
    // and bindings. Destroyed after operation_scope_ so outstanding operations
    // still see request-owned objects.
    mutable request_storage_owner_type request_storage_;
    std::exception_ptr error_;
    mutable bool body_decoded_ : 1 {false};
    // Content-Encoding was parsed and is empty: request_body() returns the raw
    // bytes without re-reading the header.
    mutable bool body_identity_ : 1 {false};

    [[nodiscard]] const detail::context_request_body_source& request_body_source() const noexcept;
    [[nodiscard]] detail::context_response_output& response_output() noexcept;
    [[nodiscard]] const detail::context_response_output& response_output() const noexcept;
    [[nodiscard]] detail::context_response_state& response_state() noexcept;
    [[nodiscard]] const detail::context_response_state& response_state() const noexcept;
    [[nodiscard]] detail::context_session_state& session_state() noexcept;
    [[nodiscard]] const detail::context_session_state& session_state() const noexcept;
    [[nodiscard]] detail::request_bindings& request_bindings() noexcept;
    [[nodiscard]] const detail::request_bindings& request_bindings() const noexcept;

    // Declared last so it closes first, while every request-owned object and its
    // memory resource are still alive.
    mutable ::ruvia::operation_scope operation_scope_;
};

enum class readiness_state : std::uint8_t {
    ready,
    unavailable,
};

struct readiness_response_options final {
    readiness_state state_{readiness_state::ready};
    borrowed_text unavailable_reason_{"service is not ready"};
};

[[nodiscard]] http_response make_health_response(context& context_value);

[[nodiscard]] http_response make_readiness_response(
    context& context_value, readiness_response_options options = {});

template <typename t_type>
inline request_state_binding<t_type> context::bind_request_state(const t_type& value) {
    return request_bindings().bind_state(value);
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>& context::request_state() const {
    return request_bindings().get_state<t_type>();
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
inline http_response context::json(const t_type& value) {
    auto body = to_json(value);
    return json_serialized(body);
}

template <typename t_type>
inline std::optional<t_type> context_request::parse_typed(
    std::optional<std::string_view> raw, void (*fail)()) {
    if (!raw) {
        return std::nullopt;
    }
    if constexpr (std::same_as<t_type, bool>) {
        const auto parsed_value = detail::parse_form_bool(*raw);
        if (!parsed_value) {
            fail();
        }
        return *parsed_value;
    } else {
        const auto parsed_value = parse_integer<t_type>(*raw);
        if (parsed_value.index() != 0) {
            fail();
        }
        return std::get<0>(parsed_value);
    }
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::query(std::string_view name) const {
    return parse_typed<t_type>(query(name), detail::throw_invalid_query);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::query(std::string_view name) const {
    return parse_typed<t_type>(query(name), detail::throw_invalid_query);
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::param(std::string_view name) const {
    return parse_typed<t_type>(param(name), detail::throw_invalid_param);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::param(std::string_view name) const {
    return parse_typed<t_type>(param(name), detail::throw_invalid_param);
}

namespace detail {

template <validation_target target, typename model_type>
[[nodiscard]] task<std::optional<model_type>> parse_optional_request_model(
    const context& context_value, operation_options operation) {
    if (!request_has_body_media_type<target>(context_value.req().header("Content-Type"))) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_value.req().text(std::move(operation));
    co_return parse_and_validate_request_model<target, model_type>(request_body);
}

}  // namespace detail

template <typename t_type>
task<std::optional<t_type>> context_request::json_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(context_->operation_scope_,
        detail::parse_optional_request_model<validation_target::json, t_type>(*context_, std::move(operation)));
}

template <typename t_type>
task<std::optional<t_type>> context_request::form_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(context_->operation_scope_,
        detail::parse_optional_request_model<validation_target::form, t_type>(*context_, std::move(operation)));
}

template <typename t_type>
inline const t_type& context_request::validated() const {
    return context_->request_bindings().get_validated<t_type>();
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
inline request_binding_guard<t_type> bind_validated_model(context& context_value, const t_type& model) {
    return context_value.request_bindings().bind_validated(model);
}

template <validation_target target>
[[nodiscard]] const request_name_value_list& validation_target_fields(context& c) {
    if constexpr (target == validation_target::query) {
        return c.req().query_fields();
    } else if constexpr (target == validation_target::param) {
        return c.req().param_fields();
    } else if constexpr (target == validation_target::header) {
        return c.req().header_fields();
    } else if constexpr (target == validation_target::cookie) {
        return c.req().cookie_fields();
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>,
            "unsupported validator target");
    }
}

template <validation_target target, typename body_type>
task<void> model_validation_middleware<target, body_type>::invoke(context& c, next& next_value) {
    if constexpr (target == validation_target::json || target == validation_target::form) {
        if (!request_has_body_media_type<target>(c.req().header("Content-Type"))) {
            throw_invalid_content_type<target>();
        }
        const auto request_body = co_await c.req().text();
        auto body = parse_and_validate_request_model<target, body_type>(request_body);
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    } else {
        auto body = parse_and_validate_request_model<target, body_type>(
            std::cref(validation_target_fields<target>(c)));
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    }
}

}  // namespace ruvia::detail
