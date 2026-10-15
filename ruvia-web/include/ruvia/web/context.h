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
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/attributes.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/conn_info.h"
#include "ruvia/web/context_request.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/dispatch.h"
#include "ruvia/web/error.h"
#include "ruvia/web/request_fields.h"
#include "ruvia/web/static_text.h"

namespace ruvia {

class context;
class env;
class static_root;
// Capabilities a handler reaches through context; include the defining header
// where they are used: http_client_handle.h, websocket.h, http_tunnel.h,
// session.h, streaming.h and multipart_reader.h.
class http_client_handle;
class websocket;
class http_tunnel;
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
class request_bindings;
template <typename t_type>
class request_binding_handle;
class request_query_values;
enum class static_file_selection_mode : std::uint8_t;
struct context_access;
class context_services;
struct session_access;
}  // namespace detail

template <typename t_type>
using request_state_binding_type = detail::request_binding_handle<t_type>;

struct redirect_response_options final {
    // URI reference: preserves existing %HH escapes and the first '#' separator;
    // subsequent '#' bytes are encoded as %23. CR and LF are rejected.
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
    template <typename t_type>
    friend detail::request_binding_handle<t_type> detail::bind_validated_json_model(
        context& context_value, const t_type& model, std::string_view raw_json);

    context(request_memory& memory, const http_request& request, detail::context_services services);

    context(request_memory& memory, const http_request& request, std::string_view route_path,
        const std::string_view* param_names, const std::string_view* param_values,
        std::size_t param_count, std::uintptr_t route_rate_limit_scope,
        detail::context_services services);

    [[nodiscard]] http_response static_file(const static_root& root, static_file_response_options options,
        detail::static_file_selection_mode mode) const;

public:
    using header_options_type = http_response_header_options;

    // Sends a bodyless 1xx head before the final response. Fields are copied
    // before return; the operation and its storage retire on the owning worker.
    // Output is linear when started. Unstarted operations may coexist and do not
    // block the final response; starting one after the final head commits throws.
    // A failed write throws std::system_error, which is not routed through
    // on_error: the connection ends.
    [[nodiscard]] scoped_operation<void> inform(const http_interim_response_head& response);

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
    // db(), redis(), get_http_client({...}).send(), and run_blocking() bind this
    // token automatically. Explicit operation tokens are combined with it.
    //
    // Cancellation is cooperative: framework waits, including streaming sleep(),
    // observe this token; CPU work and raw Asio waits without it are not preempted.
    // Connection phase timeouts close the socket independently.
    //
    // HTTP/1 cannot observe a peer FIN while a handler is suspended without a
    // concurrent transport read, so this is not a promise of immediate
    // client-disconnect detection.
    //
    [[nodiscard]] stop_token get_stop_token() const noexcept {
        return capabilities_.stop_token();
    }

    // Present only while session_middleware is bound for this request.
    [[nodiscard]] ruvia::session session();
    [[nodiscard]] std::optional<ruvia::session> try_session() noexcept;

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
    [[nodiscard]] request_state_binding_type<t_type> bind_request_state(const t_type& value);

    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    [[nodiscard]] request_state_binding_type<std::remove_cvref_t<t_type>> bind_request_state(t_type&&) = delete;

    // Throws std::logic_error when nothing of this type is bound. Use
    // try_request_state<T>() where absence is a normal outcome -- an optional
    // auth middleware, say.
    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& request_state() const;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_request_state() const noexcept;

    [[nodiscard]] const ruvia::env& env() const noexcept;

    // Re-enter the immutable route table on this worker. Authentication,
    // validation and middleware run in a fresh request context and arena.
    [[nodiscard]] scoped_operation<dispatch_response> dispatch(dispatch_options options);
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
    [[nodiscard]] http_client_handle get_http_client() const;
    [[nodiscard]] http_client_handle get_http_client(std::string_view alias) const;
    [[nodiscard]] websocket& get_websocket() const;
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

    void header(std::string_view name, std::string_view value) {
        header(name, value, header_options_type{});
    }

    void header(std::string_view name, std::string_view value, header_options_type options);
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
    // static_text, which explicitly borrows static bytes without copying.
    // Character arrays (string literals and filled buffers) are copied up to
    // their first NUL byte, or in full when they contain none.
    [[nodiscard]] http_response body(std::string_view body) const;
    [[nodiscard]] http_response body(std::nullptr_t) const;
    [[nodiscard]] http_response body(std::pmr::string&& body) const;
    [[nodiscard]] http_response body(std::span<const std::byte> body) const;
    [[nodiscard]] http_response body(static_text body) const;
    [[nodiscard]] http_response body(std::string& body) const = delete;
    [[nodiscard]] http_response body(const std::string& body) const = delete;
    [[nodiscard]] http_response body(std::string&& body) const = delete;

    template <std::size_t n>
    [[nodiscard]] http_response body(const char (&body)[n]) const;

    [[nodiscard]] http_response text(std::string_view body) const;
    [[nodiscard]] http_response text(std::pmr::string&& body) const;
    [[nodiscard]] http_response text(static_text body) const;
    [[nodiscard]] http_response text(std::string& body) const = delete;
    [[nodiscard]] http_response text(const std::string& body) const = delete;
    [[nodiscard]] http_response text(std::string&& body) const = delete;

    template <std::size_t n>
    [[nodiscard]] http_response text(const char (&body)[n]) const;

    template <typename t_type>
        requires detail::is_model<t_type>
    [[nodiscard]] http_response json(const t_type& value) const;

    [[nodiscard]] http_response html(std::string_view body) const;
    [[nodiscard]] http_response html(std::pmr::string&& body) const;
    [[nodiscard]] http_response html(static_text body) const;
    [[nodiscard]] http_response html(std::string& body) const = delete;
    [[nodiscard]] http_response html(const std::string& body) const = delete;
    [[nodiscard]] http_response html(std::string&& body) const = delete;

    template <std::size_t n>
    [[nodiscard]] http_response html(const char (&body)[n]) const;

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
    // ".." is clamped at the root; an absolute path, a NUL, or an ascent above
    // the root is 403, and an unindexed name 404.
    [[nodiscard]] http_response static_file(
        const static_root& root, static_file_response_options options) const;

    [[nodiscard]] http_response error(http_error_info_options options) const;

    [[nodiscard]] scoped_operation<http_response> not_found();

private:
    [[nodiscard]] http_response streaming_head() const;

    [[nodiscard]] task<http_response> not_found_task();
    // `fields`: method, target, body, then header name/value pairs (owned copies).
    [[nodiscard]] task<dispatch_response> dispatch_task(
        std::pmr::vector<std::pmr::string> fields, stop_token stop);
    [[nodiscard]] task<std::string_view> request_body() const;
    task<void> request_discard_body() const;
    [[nodiscard]] task<std::pmr::vector<multipart_part>> request_multipart() const;
    [[nodiscard]] body_reader& request_body_reader() const;
    [[nodiscard]] multipart_reader request_multipart_reader() const;
    [[nodiscard]] std::optional<std::string_view> route_param(std::string_view name) const;
    void ensure_route_params() const;
    [[nodiscard]] bool request_accepts(std::string_view media_type) const noexcept;
    [[nodiscard]] std::optional<std::string_view> request_negotiate(context_request::negotiable_type field,
        std::span<const std::string_view> supported) const noexcept;
    void ensure_request_query() const;
    [[nodiscard]] std::optional<std::string_view> request_query(std::string_view name) const;
    [[nodiscard]] const request_name_value_list& request_query() const;
    [[nodiscard]] const detail::request_query_values& request_queries() const;
    [[nodiscard]] std::optional<std::string_view> request_cookie(std::string_view name) const;
    [[nodiscard]] const request_name_value_list& request_cookies() const;
    [[nodiscard]] multipart_boundary get_multipart_boundary() const;

    [[nodiscard]] bool request_content_type_matches(std::string_view expected) const noexcept;

    context& set_stable_response_header(std::string_view name, std::string_view value);
    void apply_response_state(http_response& response, std::optional<http_status_code> status_code) const;

    [[nodiscard]] http_response json_serialized(std::pmr::string& body) const;

    [[nodiscard]] const request_name_value_list& request_headers() const;
    [[nodiscard]] std::optional<std::string_view> request_header(std::string_view name) const;
    [[nodiscard]] const request_name_value_list& route_params() const;
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

namespace detail {

template <typename t_type>
request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model);

template <typename t_type>
request_binding_handle<t_type> bind_validated_json_model(
    context& context_value, const t_type& model, std::string_view raw_json);

}  // namespace detail

}  // namespace ruvia

#include "ruvia/web/detail/http/context/context.inl"
#include "ruvia/web/detail/http/context/context_model.inl"
