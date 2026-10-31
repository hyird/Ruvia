#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"
#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
namespace ruvia {

class context;
class env;
class static_root;
// Capabilities a handler reaches through context; include the defining header
// where they are used: http_client.h, websocket.h, http_tunnel.h,
// session.h, streaming.h and multipart_reader.h.
class http_client_handle;
class websocket;
class http_tunnel;
class test_app;
class session;
class body_reader;
class response_stream_writer;
class sse_writer;
class multipart_reader;
class multipart_part;
class multipart_boundary;
#ifdef RUVIA_ENABLE_DATABASE
class db_handle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_handle;
#endif
namespace detail {
class context_request_storage;
class context_request_body_source;
class context_response_output;
class context_response_state;
class context_session_state;
template <typename t_type>
[[nodiscard]] request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model);
template <typename t_type>
    requires(!std::is_lvalue_reference_v<t_type>)
request_binding_handle<std::remove_cvref_t<t_type>> bind_validated_model(context&, t_type&&) = delete;

[[noreturn]] void throw_invalid_json_content_type();
[[noreturn]] void throw_invalid_json_body();
[[noreturn]] void throw_invalid_form_content_type();
[[noreturn]] void throw_invalid_form_body();
[[noreturn]] void throw_invalid_query();
[[noreturn]] void throw_invalid_param();
[[noreturn]] void throw_invalid_header();
[[noreturn]] void throw_invalid_cookie();

class request_query_values;
enum class static_file_selection_mode : std::uint8_t;
struct context_access;
class context_services;
struct session_access;
struct request_name_value_view_access;
struct request_name_value_list_access;
}  // namespace detail

class plain_connection_transport final {
private:
    friend class conn_info;
    constexpr plain_connection_transport() noexcept = default;
};

class tls_connection_transport final {
public:
    // The verified peer certificate subject DN for mutual TLS, or empty when
    // no client certificate was presented.
    [[nodiscard]] constexpr std::string_view client_certificate_subject() const noexcept {
        return client_certificate_subject_;
    }

private:
    friend class conn_info;

    explicit constexpr tls_connection_transport(std::string_view client_certificate_subject) noexcept
        : client_certificate_subject_(client_certificate_subject) {}

    std::string_view client_certificate_subject_;
};

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside http_request and context_request.
class conn_info final {
public:
    class endpoint final {
    public:
        [[nodiscard]] constexpr std::string_view address() const noexcept {
            return address_;
        }

        // Zero when the port is not known: a forwarded client address whose
        // proxy did not record one, or a hand-built test connection.
        [[nodiscard]] constexpr std::uint16_t port() const noexcept {
            return port_;
        }

    private:
        friend class conn_info;

        explicit constexpr endpoint(std::string_view address, std::uint16_t port = 0) noexcept
            : address_(address),
              port_(port) {}

        std::string_view address_;
        std::uint16_t port_{0};
    };

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use client() for the caller.
    [[nodiscard]] constexpr endpoint remote() const noexcept {
        return remote_;
    }

    // Who the request is from. Equal to remote() unless the peer is a configured
    // trusted proxy AND it sent a forwarding header, in which case it is what
    // that header names. Never derived from an untrusted peer's headers: those
    // are attacker-controlled, so with no trusted proxy configured this is
    // always the direct peer.
    //
    // This, not remote(), is what rate limiting keys on and what an access log
    // should record.
    [[nodiscard]] constexpr endpoint client() const noexcept {
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

    [[nodiscard]] constexpr const plain_connection_transport* plain() const& noexcept {
        return std::get_if<plain_connection_transport>(&transport_);
    }
    const plain_connection_transport* plain() const&& = delete;

    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept {
        return std::get_if<tls_connection_transport>(&transport_);
    }
    const tls_connection_transport* tls() const&& = delete;

private:
    friend class detail::context_services;

    constexpr conn_info(std::string_view remote_address, plain_connection_transport transport) noexcept
        : remote_(remote_address),
          client_(remote_address),
          transport_(transport),
          scheme_(http_scheme::http) {}

    constexpr conn_info(std::string_view remote_address, tls_connection_transport transport) noexcept
        : remote_(remote_address),
          client_(remote_address),
          transport_(transport),
          scheme_(http_scheme::https) {}

    [[nodiscard]] static constexpr conn_info plain(std::string_view remote_address) noexcept {
        return conn_info(remote_address, plain_connection_transport{});
    }

    [[nodiscard]] static constexpr conn_info tls(
        std::string_view remote_address, std::string_view client_certificate_subject) noexcept {
        return conn_info(remote_address, tls_connection_transport(client_certificate_subject));
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty argument leaves the transport-derived value, so a
    // proxy that sends one field but not the other does not blank the rest.
    constexpr void apply_forwarded(
        std::string_view client_address, std::string_view forwarded_scheme) noexcept {
        if (!client_address.empty()) {
            client_ = endpoint(client_address);
            via_trusted_proxy_ = true;
        }
        if (forwarded_scheme == "http") {
            scheme_ = http_scheme::http;
            via_trusted_proxy_ = true;
        } else if (forwarded_scheme == "https") {
            scheme_ = http_scheme::https;
            via_trusted_proxy_ = true;
        }
    }

    constexpr void set_remote_port(std::uint16_t port) noexcept {
        remote_ = endpoint(remote_.address(), port);
        if (!via_trusted_proxy_) {
            client_ = remote_;
        }
    }

    endpoint remote_;
    endpoint client_;
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
    std::string_view method_{};
    std::string_view target_{};
    std::span<const http_header_view> headers_{};
    std::string_view body_{};
    stop_token stop_token_{};
};

// Owns its bytes in the calling worker's reclaimable pool (or process resource
// for test_app), independently of the child request arena.
class dispatch_response final {
public:
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

private:
    friend class context;
    friend class test_app;
    explicit dispatch_response(std::pmr::memory_resource* resource = nullptr)
        : headers_(pmr_resource_or_default(resource)),
          body_(pmr_resource_or_default(resource)) {}
    http_status_code status_{http_status::ok};
    std::pmr::vector<http_header> headers_;
    std::pmr::string body_;
};

template <typename t_type>
using request_state_binding = detail::request_binding_handle<t_type>;

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

    // Materialized once in request order, preserving duplicates. Header names
    // retain their semantic spelling; header get/count
    // and header_model binding compare names ASCII case-insensitively.
    [[nodiscard]] const request_name_value_list& header_fields() const;
    [[nodiscard]] const request_name_value_list& query_fields() const;
    [[nodiscard]] const request_name_value_list& cookie_fields() const;
    [[nodiscard]] const request_name_value_list& param_fields() const;
    [[nodiscard]] std::optional<std::string_view> signed_cookie(signed_cookie_lookup_options options) const;

    // Buffered views borrow context storage; read the media type with header("Content-Type").
    [[nodiscard]] task<std::string_view> text() const;
    [[nodiscard]] task<std::span<const std::byte>> bytes() const;
    task<void> discard_body() const;

    // Typed optional parsing with schema and field-rule validation. Only a
    // media-type mismatch yields nullopt; selected but malformed input throws
    // HTTP 400, and rule violations throw an http_error carrying the issues.
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> json_if() const;
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> form_if() const;
    template <typename t_type>
    [[nodiscard]] const t_type& validated() const;

    // Multipart remains a flat protocol representation, without dotted paths
    // or implicit array/group interpretation. Streaming is explicit-route only.
    // multipart() decodes a supported Content-Encoding; multipart_reader()
    // decodes none, so a coded body is refused with 415 and an invalid
    // Content-Encoding field with 400.
    [[nodiscard]] task<std::pmr::vector<multipart_part>> multipart() const;
    [[nodiscard]] ::ruvia::body_reader& body_reader() const;
    [[nodiscard]] ::ruvia::multipart_reader multipart_reader() const;

private:
    friend class context;
    explicit constexpr context_request(const context& context_value RUVIA_LIFETIMEBOUND) noexcept
        : context_(&context_value) {}

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

struct static_file_response_options final {
    borrowed_text relative_path_{};
    borrowed_text content_type_{};
};

// The file context::file() serves, borrowed for that call: UTF-8 text, or a
// std::filesystem::path lvalue whose native string is used as is. Neither form
// builds a std::filesystem::path per request; text is NUL-terminated (and
// widened to UTF-16 on Windows) in the request arena. Like borrowed_text, it
// refuses owning temporaries, so a path assembled per request has to be named
// by the caller -- hoist it out of the handler when it is fixed.
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

struct file_response_options final {
    borrowed_file_path path_{};
    borrowed_text content_type_{};
};

class context final {
private:
    friend class context_request;
    friend struct detail::context_access;
    friend struct detail::session_access;
    template <typename t_type>
    friend detail::request_binding_handle<t_type> detail::bind_validated_model(
        context& context_value, const t_type& model);

    context(request_memory& memory, const http_request& request, const detail::context_services& services);

    context(request_memory& memory, const http_request& request, std::string_view route_path,
        const std::string_view* param_names, const std::string_view* param_values,
        std::size_t param_count, std::uintptr_t route_rate_limit_scope,
        const detail::context_services& services);

    [[nodiscard]] http_response static_file(const static_root& root, static_file_response_options options,
        detail::static_file_selection_mode mode) const;

public:
    // Sends a bodyless 1xx head before the final response. Fields are copied
    // before return; the operation and its storage retire on the owning worker.
    // Output is linear when started. Unstarted operations may coexist and do not
    // block the final response; starting one after the final head commits throws.
    // A failed write throws std::system_error, which is not routed through
    // on_error: the connection ends.
    [[nodiscard]] task<void> inform(const http_interim_response_head& response);

    // Queues a promised request on this HTTP/2 connection. False means the
    // peer disabled push or its stream capacity cannot admit the promise.
    [[nodiscard]] bool push(http_push_request_view request);
    // Connection-scoped HTTP/2 advertisements; ORIGIN requires TLS.
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
    [[nodiscard]] conn_info conn() const& noexcept RUVIA_LIFETIMEBOUND {
        return conn_info_;
    }
    conn_info conn() const&& = delete;

    // The exception that failed the current middleware/handler dispatch, or
    // null. Distinct from error(status, code, message) which constructs an
    // error response.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return error_;
    }

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return capabilities_.worker();
    }

    // Borrows this worker's registered instance; never hand it to another
    // worker. An unregistered type throws std::logic_error.
    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable executes on the blocking pool and must own everything it
    // touches. Completion resumes on this worker; timeout or shutdown stops
    // the wait without joining a still-running foreign callable.
    // Rejection and callable exceptions are retained in blocking_result. Inspect
    // status() for load shedding or use std::move(result).value() to throw.
    // A disabled pool is a configuration error and throws before submission.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable) const {
        return capabilities_.run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return capabilities_.run_blocking(timeout, std::move(callable));
    }

    // Observes worker shutdown and, for a subrequest, explicit cancellation.
    // db(), redis(), http_client({...}).send(), and run_blocking() bind this
    // token automatically. Explicit operation tokens are combined with it.
    //
    // Cancellation is cooperative: framework waits, including streaming sleep(),
    // observe this token; CPU work and raw Asio waits without it are not preempted.
    // Connection phase timeouts close the socket independently.
    //
    // HTTP/1 默认不会在 handler 挂起时检测 peer FIN。长驻 SSE 可显式调用
    // sse_writer::cancel_on_peer_eof()，并把其返回的组合 token 传给业务等待；
    // 此处返回的原 token 不变。该选项将合法写半关闭也视作放弃响应。
    //
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept {
        return capabilities_.stop_token();
    }

    // Present only while session_middleware is bound for this request.
    [[nodiscard]] ruvia::session session();

    // Request and handshake lifetime resource. Allocations remain in the
    // request arena until the owning request_memory is destroyed and are not
    // individually reclaimed.
    [[nodiscard]] std::pmr::memory_resource* arena() const noexcept {
        return memory_.resource();
    }

    // Reclaimable worker-local storage for temporary owning objects. Destroy
    // them on this worker within the context's scope. Deallocation returns
    // storage to the pool for reuse without invalidating other live objects;
    // it need not return memory to the OS. Clients may own distinct pools, so
    // transferring owned data to an operation only avoids copying when its
    // destination resource is compatible.
    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept {
        return memory_.upstream_resource();
    }

    // Request-scoped typed state: how a middleware hands a value it computed --
    // an authenticated user, a resolved tenant, a trace span -- to everything it
    // calls through next(). The counterpart of worker_state<T>(), which lives for
    // the worker's whole life and is shared by every request on it; this lives
    // for one dynamic next() scope and is private to that request.
    //
    //   task<void> handle(context& c, next& next_value) {
    //       const auto user = authenticate(c);          // owned by this frame
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

    [[nodiscard]] const ruvia::env& env() const noexcept;

    // Re-enter the immutable route table on this worker. Authentication,
    // validation and middleware run in a fresh request context and arena.
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
    [[nodiscard]] std::pmr::string url_for(
        std::string_view pattern, std::initializer_list<std::string_view> values = {}) const;

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_handle db() const;
    [[nodiscard]] db_handle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_handle redis() const;
    [[nodiscard]] redis_handle redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_handle http_client() const;
    [[nodiscard]] http_client_handle http_client(std::string_view alias) const;
    [[nodiscard]] ::ruvia::websocket& websocket() const;
    [[nodiscard]] http_tunnel& tunnel() const;

    [[nodiscard]] response_stream_writer& stream();

    [[nodiscard]] response_stream_writer& stream_text();

    [[nodiscard]] sse_writer stream_sse();

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<t_type>(arena());
    }

    // Route handlers construct one final response, so context accepts only
    // 200..599. Informational heads belong to a dedicated protocol submit path.
    // Status, headers and cookies set before a final response exists are the
    // metadata every response built from this context starts from. Set after a
    // final response exists (for example after next()), they post-process that
    // response and also become the metadata of a response that replaces it.
    void status(http_status_code status_code);

    void header(std::string_view name, std::string_view value, http_response_header_options options = {});
    // Response priority hints for downstream intermediaries. Absent members
    // stay absent so the intermediary can merge them with client parameters.
    void priority(http_priority_fields fields);

    // Remove a response header set by this handler, before the response is
    // committed. Setting header(name, std::nullopt) to mean deletion was a
    // hidden sentinel; removal now has its own named entry point.
    void remove_header(std::string_view name);

    void set_cookie(set_cookie_options options);
    void set_signed_cookie(set_signed_cookie_options options);
    void delete_cookie(delete_cookie_options options);

    // Observe the final response produced by downstream middleware or a terminal
    // handler. Internal provisional response storage is never exposed here.
    [[nodiscard]] const http_response* response() const noexcept;

    // End middleware dispatch with an explicitly constructed response. Calling
    // next() after respond() is a control-flow error and becomes a 500 response;
    // a middleware may still call respond() after next() to replace an
    // uncommitted downstream response. A replacement -- and an error raised
    // after next() -- carries none of the replaced response's status or headers,
    // only the context metadata set after that response became final.
    void respond(http_response&& response);

    // Body builders copy their input into response-owned storage, except
    // static_text, which explicitly borrows static bytes without copying, and
    // std::pmr::string&&, which transfers compatible arena allocations.
    [[nodiscard]] http_response body(std::string_view body = {}) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] http_response body(string_type&& body_value) const {
        return body_owned(std::move(body_value));
    }
    [[nodiscard]] http_response body(std::span<const std::byte> body) const;
    [[nodiscard]] http_response body(static_text body) const;

    [[nodiscard]] http_response text(std::string_view body) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] http_response text(string_type&& body_value) const {
        return text_owned(std::move(body_value));
    }
    [[nodiscard]] http_response text(static_text body) const;

    template <typename t_type>
        requires detail::is_model<t_type>
    [[nodiscard]] http_response json(const t_type& value) const;

    [[nodiscard]] http_response html(std::string_view body) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] http_response html(string_type&& body_value) const {
        return html_owned(std::move(body_value));
    }
    [[nodiscard]] http_response html(static_text body) const;
    [[nodiscard]] http_response redirect(redirect_response_options options) const;

    // File helpers honor If-Range only for matching strong ETags. Last-Modified
    // dates are weak and do not authorize a partial response.
    // file() opens the file once and the response owns that handle; plain-TCP
    // transfers send from it without resolving the path again. The handle is
    // checked again right before sending: a file changed in place since file()
    // fails a single plain-TCP body before its head leaves and aborts any
    // other transfer before its bytes.
    [[nodiscard]] http_response file(file_response_options options) const;

    // relative_path_ is a decoded file path below the root, such as
    // req().param("*") of a wildcard route; it is never percent-decoded again.
    // '/' and '\\' separate segments. A "." or ".." segment, an empty interior
    // segment, an absolute path, or a NUL is 403 -- rejected, never resolved,
    // whichever parameter it came from -- and an unindexed name 404.
    [[nodiscard]] http_response static_file(
        const static_root& root, static_file_response_options options) const;

    [[nodiscard]] http_response error(http_error_info_options options) const;

    [[nodiscard]] task<http_response> not_found();

private:
    [[nodiscard]] http_response streaming_head() const;
    [[nodiscard]] http_response body_owned(std::pmr::string&& body) const;
    [[nodiscard]] http_response text_owned(std::pmr::string&& body) const;
    [[nodiscard]] http_response html_owned(std::pmr::string&& body) const;

    [[nodiscard]] task<http_response> not_found_task();
    [[nodiscard]] task<dispatch_response> dispatch_task(std::pmr::string method,
        std::pmr::string target, std::pmr::string body, std::pmr::vector<http_header> headers,
        ::ruvia::stop_token stop);
    context& set_stable_response_header(std::string_view name, std::string_view value);
    void apply_response_state(http_response& response, std::optional<http_status_code> status_code) const;

    [[nodiscard]] http_response json_serialized(std::pmr::string& body) const;

    [[nodiscard]] std::pmr::string& decoded_body() const;
    [[nodiscard]] detail::context_request_storage& request_storage() const;
    [[nodiscard]] detail::context_services& services() noexcept;
    [[nodiscard]] const detail::context_services& services() const noexcept;
    [[nodiscard]] http_response& response_storage();
    void store_response(http_response&& response);
    void store_assigned_response(http_response&& response);
    void store_error(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool has_response() const noexcept;

    request_memory& memory_;
    const http_request& request_;
    conn_info conn_info_;
    // context cannot escape request dispatch and therefore borrows the stable
    // server-owned handle without touching its shared ownership count.
    detail::worker_context_capabilities capabilities_;
    std::string_view route_path_;
    const std::string_view* param_names_{nullptr};
    const std::string_view* param_values_{nullptr};
    std::size_t param_count_{0};
    std::uintptr_t route_rate_limit_scope_{0};
    using request_storage_owner_type = std::unique_ptr<detail::context_request_storage,
        pmr_object_deleter<detail::context_request_storage>>;
    // One typed arena allocation owns request caches, response/session state,
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

}  // namespace ruvia

#include "ruvia/web/detail/http/context/context.inl"
