#pragma once

#include <chrono>
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
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/attributes.h"
#include "ruvia/web/conn_info.h"
#include "ruvia/web/context_request.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/dispatch.h"
#include "ruvia/web/error.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/http3_early_data_info.h"
#include "ruvia/web/http_client_handle.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/model_types.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/request_fields.h"
#include "ruvia/web/session.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/task.h"
#include "ruvia/web/validation_types.h"
#include "ruvia/web/websocket.h"

namespace ruvia {

class context;
class env;
class static_root;
class http_client_handle;
class http_request_trailers;

#ifdef RUVIA_ENABLE_DATABASE
class db_handle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_handle;
#endif
namespace detail {
struct steady_rate_limiter_clock;
template <typename clock_type>
class rate_limiter;
using rate_limiter_type = rate_limiter<steady_rate_limiter_clock>;
class route_table;
class worker_state_registry;
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
class request_deadline;
class http_interim_response_output;
class http_connection_advertisement_output;
class http_push_output;
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

struct file_response_options final {
    std::filesystem::path path_{};
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
    using header_options_type = http_response::header_options_type;

    // Dispatches the promised GET/HEAD through the same route/middleware plan.
    // Returns false if peer push permission/capacity is unavailable. Completion
    // commits the promise; the connection owns and joins its response task.
    [[nodiscard]] scoped_operation<bool> push(http_push_request_view request);
    // Sends a bodyless 1xx head before the final response. Fields are copied
    // before return; the operation and its storage retire on the owning worker.
    [[nodiscard]] scoped_operation<void> inform(const http_interim_response_head& response);
    [[nodiscard]] scoped_operation<void> advertise_origins(std::span<const std::string_view> origins);
    [[nodiscard]] scoped_operation<void> advertise_alternative_service(std::string_view value);

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

    // Immutable snapshot for this request. `received_from_early_data` is trusted
    // local QUIC transport provenance; `upstream_declared_early_data` reflects
    // only the untrusted HTTP field and must not be used as proof of 0-RTT.
    [[nodiscard]] http3_early_data_info early_data_info() const noexcept {
        return early_data_info_;
    }

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
    template <typename callable_type>
    [[nodiscard]] task<std::invoke_result_t<callable_type&>> run_blocking(callable_type callable) const {
        return capabilities_.run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<std::invoke_result_t<callable_type&>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return capabilities_.run_blocking(timeout, std::move(callable));
    }

    // Pool rejection is returned as a status; exceptions from the callable
    // still propagate. A disabled pool is a configuration error and throws.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> try_run_blocking(callable_type callable) const {
        return capabilities_.try_run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> try_run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return capabilities_.try_run_blocking(timeout, std::move(callable));
    }

    // Whether this request's handler deadline elapsed. The token alone cannot
    // say: it trips for worker shutdown too, and a handler that catches the
    // cancellation its own await raised needs to tell those apart before
    // deciding whether to press on.
    [[nodiscard]] bool deadline_exceeded() const noexcept;

    // Stopped when this request should stop doing work: its handler deadline
    // elapsed (see ruvia::Deadline), or the owning worker began stopping.
    // db(), redis(), get_http_client({...}).send(), and run_blocking() bind this
    // token automatically, which is how a deadline reaches a handler that
    // never mentions one. An explicit operation token is combined with it.
    //
    // Cooperative by necessity: a suspended coroutine cannot be abandoned in
    // C++, so this stops the WAITS rather than the handler. Every wait Ruvia
    // hands a handler observes this token, streaming sleep() included; a wait the
    // application built out of raw Asio without one is not stopped, and if it is
    // still suspended while the worker can run, the connection scanner's current
    // inactivity phase eventually drops the socket instead of answering on it.
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
    //   task<void> handle(context& c, Next& next) {
    //       const auto user = authenticate(c);          // owned by this frame
    //       const auto binding = c.bind_request_state(user);
    //       co_await next();                            // handler sees it
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
    // the value for a trailing "*" keeps its slashes and may be empty. Throws
    // std::invalid_argument for an unregistered pattern or a value-count
    // mismatch, and std::logic_error when the context carries no route table
    // (for example a hand-built test context).
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
    // uncommitted downstream response.
    void respond(http_response&& response);

    [[nodiscard]] http_response body(std::string_view body) const;
    [[nodiscard]] http_response body(std::nullptr_t) const;
    [[nodiscard]] http_response body(std::pmr::string&& body) const;
    [[nodiscard]] http_response body(std::span<const std::byte> body) const;
    [[nodiscard]] http_response body(std::string& body) const = delete;
    [[nodiscard]] http_response body(const std::string& body) const = delete;
    [[nodiscard]] http_response body(std::string&& body) const = delete;

    template <std::size_t n>
    [[nodiscard]] http_response body(const char (&body)[n]) const;

    [[nodiscard]] http_response text(std::string_view body) const;
    [[nodiscard]] http_response text(std::pmr::string&& body) const;
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
    [[nodiscard]] http_response html(std::string& body) const = delete;
    [[nodiscard]] http_response html(const std::string& body) const = delete;
    [[nodiscard]] http_response html(std::string&& body) const = delete;

    template <std::size_t n>
    [[nodiscard]] http_response html(const char (&body)[n]) const;

    [[nodiscard]] http_response redirect(redirect_response_options options) const;

    // File helpers honor If-Range only for matching strong ETags. Last-Modified
    // dates are weak and do not authorize a partial response.
    [[nodiscard]] http_response file(file_response_options options) const;

    [[nodiscard]] http_response static_file(
        const static_root& root, static_file_response_options options) const;

    [[nodiscard]] http_response error(http_error_info_options options) const;

    [[nodiscard]] scoped_operation<http_response> not_found();

private:
    [[nodiscard]] http_response streaming_head(std::string_view content_type = {}) const;

    [[nodiscard]] task<http_response> not_found_task();
    [[nodiscard]] task<dispatch_response> dispatch_task(std::pmr::string request, operation_options options);
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
    context& remove_response_header(std::string_view name);
    void apply_response_state(http_response& response, std::optional<http_status_code> status_code) const;

    [[nodiscard]] http_response body_static_view(std::string_view body) const;
    [[nodiscard]] http_response text_static_view(std::string_view body) const;
    [[nodiscard]] http_response html_static_view(std::string_view body) const;

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
    [[nodiscard]] http_response take_response();

    request_memory& memory_;
    const http_request& request_;
    http3_early_data_info early_data_info_{};
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
        detail::pmr_object_deleter<detail::context_request_storage>>;
    // One typed arena allocation owns request caches, response/session state,
    // and bindings. Destroyed after operation_scope_ so outstanding operations
    // still see request-owned objects.
    mutable request_storage_owner_type request_storage_;
    std::exception_ptr error_;
    mutable bool body_decoded_ : 1 {false};

    [[nodiscard]] detail::context_request_body_source& request_body_source() noexcept;
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
