#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "connection_transport.h"
#include "decimal_text.h"
#include "http_cookie.h"
#include "http_error.h"
#include "http_header.h"
#include "http_message.h"
#include "http_priority.h"
#include "http_request.h"
#include "http_request_head_view.h"
#include "http_response.h"
#include "http_status.h"
#include "ip_address.h"
#include "middleware.h"
#include "model.h"
#include "outbound_bytes.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_services.h"
namespace ruvia {

class context;
template <typename host_type>
class request_exchange;
class env;
class static_root;
// Capabilities a handler reaches through context; include the defining header
// where they are used: http_client.h, websocket.h (websocket), http_tunnel.h
// (http_tunnel), streaming.h (multipart_reader) and session.h (session, with
// RUVIA_ENABLE_REDIS).
class http_client_ref;
class http_client_response;
class websocket;
class http_tunnel;
class test_app;
class body_reader;
class response_stream_writer;
class tls_client_hello_record;
class server_connection;
class sse_writer;
class multipart_reader;
class multipart_part;
#ifdef RUVIA_ENABLE_DATABASE
class db_executor;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_executor;
class session;
#endif
namespace detail {
class context_request_storage;
class context_request_body_source;
class context_response_output;
class context_response_state;
#ifdef RUVIA_ENABLE_REDIS
class context_session_state;
struct session_access;
#endif
template <typename t_type>
[[nodiscard]] request_binding_guard<t_type> bind_validated_model(context& context_value, const t_type& model);
template <typename t_type>
    requires(!std::is_lvalue_reference_v<t_type>)
request_binding_guard<std::remove_cvref_t<t_type>> bind_validated_model(context&, t_type&&) = delete;

[[noreturn]] void throw_invalid_query();
[[noreturn]] void throw_invalid_param();

struct context_access;
struct request_name_value_list_access;
class context_services;
}  // namespace detail

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside http_request and context_request.
class conn_info final : public connection_endpoints {
public:
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

    using transport_type = std::variant<plain_connection_transport, tls_connection_transport>;

    conn_info(const connection_endpoints& endpoints, std::string_view remote_text, transport_type transport, http_scheme scheme) noexcept :
        connection_endpoints(endpoints),
        remote_text_(remote_text),
        client_(endpoints.remote()),
        transport_(transport),
        scheme_(scheme) {}

    [[nodiscard]] static conn_info plain(const connection_endpoints& endpoints, std::string_view remote_text) noexcept {
        return conn_info(endpoints, remote_text, plain_connection_transport{}, http_scheme::http);
    }

    [[nodiscard]] static conn_info tls(const connection_endpoints& endpoints, std::string_view remote_text, const tls_connection_transport& transport) noexcept {
        return conn_info(endpoints, remote_text, transport, http_scheme::https);
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty scheme leaves the transport-derived value.
    constexpr void apply_forwarded(const ip_address& client_address, std::optional<http_scheme> forwarded_scheme) noexcept {
        client_ = ip_endpoint(client_address, 0);
        via_trusted_proxy_ = true;
        if (forwarded_scheme.has_value()) {
            scheme_ = *forwarded_scheme;
        }
    }

    // The transport's own facts replace the provisional ones; a forwarded
    // client stays.
    constexpr void assign_endpoints(const connection_endpoints& endpoints) noexcept {
        static_cast<connection_endpoints&>(*this) = endpoints;
        if (!via_trusted_proxy_) {
            client_ = remote();
        }
    }

    std::string_view remote_text_;
    ip_endpoint client_;
    transport_type transport_;
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
    // Transfers the owned bytes and leaves the body empty; metadata is unchanged.
    [[nodiscard]] std::string take_body_bytes() && noexcept {
        return std::exchange(body_, {});
    }
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

// The fields of one request source -- headers, query, cookies or path
// parameters -- in request order, duplicates preserved. Views borrow the
// context's request storage.
class request_name_value_list final {
public:
    using value_type = http_header_view;
    using const_iterator = const http_header_view*;

    request_name_value_list(const request_name_value_list&) = delete;
    request_name_value_list& operator=(const request_name_value_list&) = delete;
    request_name_value_list(request_name_value_list&& other) noexcept :
        owned_(std::move(other.owned_)),
        case_insensitive_(other.case_insensitive_) {
        items_ = owned_.empty() ? other.items_ : std::span<const http_header_view>(owned_);
        other.items_ = {};
    }
    request_name_value_list& operator=(request_name_value_list&&) = delete;

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data() + items_.size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] const http_header_view* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const http_header_view* data() const&& = delete;

    [[nodiscard]] const http_header_view& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const http_header_view& operator[](std::size_t) const&& = delete;

    // Duplicate fields are preserved in materialization order; scalar lookup uses
    // the first occurrence. Header lists compare names case-insensitively;
    // other field sources compare exactly. Enumeration preserves name spelling.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (const auto& item : items_) {
            if (names_equal(item.name(), name)) {
                return item.value();
            }
        }
        return std::nullopt;
    }

    // Security-relevant single-valued fields must not pick one of several
    // occurrences: a repeated name reports repeated instead of a value.
    [[nodiscard]] http_unique_field_lookup unique(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND {
        http_unique_field_lookup result;
        for (const auto& item : items_) {
            if (names_equal(item.name(), name)) {
                result.add_occurrence(item.value());
                if (result.status_ == unique_lookup_status::repeated) {
                    break;
                }
            }
        }
        return result;
    }
    http_unique_field_lookup unique(std::string_view) const&& = delete;

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result_value = 0;
        for (const auto& item : items_) {
            if (names_equal(item.name(), name)) {
                ++result_value;
            }
        }
        return result_value;
    }

    [[nodiscard]] std::span<const http_header_view> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const http_header_view> entries() const&& = delete;

private:
    friend struct detail::request_name_value_list_access;

    explicit request_name_value_list(bool case_insensitive = false) noexcept :
        items_(owned_),
        case_insensitive_(case_insensitive) {}
    explicit request_name_value_list(std::span<const http_header_view> headers) noexcept :
        items_(headers),
        case_insensitive_(true) {}

    [[nodiscard]] bool names_equal(std::string_view left, std::string_view right) const noexcept {
        return case_insensitive_ ? http_ascii_equals_ignore_case(left, right) : left == right;
    }

    void reserve(std::size_t count) {
        owned_.reserve(count);
        items_ = owned_;
    }

    void push_back(http_header_view value) {
        owned_.push_back(value);
        items_ = owned_;
    }

    std::vector<http_header_view> owned_{};
    std::span<const http_header_view> items_{};
    bool case_insensitive_{false};
};

namespace detail {
struct request_name_value_list_access final {
    [[nodiscard]] static request_name_value_list make() noexcept {
        return request_name_value_list();
    }
    [[nodiscard]] static request_name_value_list borrow_headers(std::span<const http_header_view> headers) noexcept {
        return request_name_value_list(headers);
    }

    static void reserve(request_name_value_list& list, std::size_t count) {
        list.reserve(count);
    }

    static void push_back(request_name_value_list& list, http_header_view value) {
        list.push_back(value);
    }
};
}  // namespace detail

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
    // Client weights select a caller-owned supported value; server order breaks
    // ties. An absent field selects the first offer, no acceptable offer is nullopt.
    [[nodiscard]] std::optional<std::string_view> negotiate(negotiable_field field, argument_list<std::string_view> offers) const noexcept;
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    // Absent / found / repeated, for fields that must not resolve one of
    // several occurrences (session ids, CSRF tokens).
    [[nodiscard]] http_unique_field_lookup unique_cookie(std::string_view name) const;
    [[nodiscard]] http_unique_field_lookup unique_header(std::string_view name) const;
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

    // Buffered views borrow context storage; collect_body() returns an owning
    // copy of the same decoded bytes. Read the media type with header("Content-Type").
    // Only one operation may load or decode the buffered body at a time.
    // Overlapping consumption throws std::logic_error.
    // `operation` bounds the read and content decoding (or the discard), even
    // when bytes are already cached. json_if(), form_if() and multipart() use
    // that same read bound before synchronous parsing. A timeout or stop ends
    // only that call with std::system_error (timed_out or operation_canceled);
    // received bytes remain available for retry. Non-positive relative timeouts
    // throw std::invalid_argument.
    // Server body timeouts still apply. This copyable handle's operations
    // borrow the context, not the handle.
    [[nodiscard]] task<std::string_view> body_text(operation_options operation = {}) const;
    [[nodiscard]] task<std::span<const std::byte>> body_bytes(operation_options operation = {}) const;
    [[nodiscard]] task<std::string> collect_body(operation_options operation = {}) const;
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
    // or implicit array/group interpretation. multipart() buffers the content
    // and decodes a supported Content-Encoding; the streamed forms are
    // context::request_body() and context::request_multipart().
    [[nodiscard]] task<std::vector<multipart_part>> multipart(operation_options operation = {}) const;

private:
    friend class context;
    explicit constexpr context_request(const context& context_value RUVIA_LIFETIMEBOUND) noexcept :
        context_(&context_value) {}

    template <typename t_type>
    [[nodiscard]] static std::optional<t_type> parse_typed(std::optional<std::string_view> raw, void (*fail)());

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

// secret_ 非空时写入 HMAC 签名值，由 context_request::signed_cookie() 读回。
// 删除 cookie：attributes_.max_age_ = 0s（写为 Expires=cookie_deletion_expires）。
struct set_cookie_options final {
    borrowed_text name_{};
    borrowed_text value_{};
    borrowed_text secret_{};
    cookie_options attributes_{};
};

// 只由接收它的调用读取，不保存。status_ 未设置时用 builder 默认值。
// headers_ 中某名称首次出现替换已有同名字段，之后的同名行追加（ASCII 不分大小写）。
// builder 自己决定的字段由调用方提供时抛 std::invalid_argument，不静默丢弃。
struct response_options final {
    std::optional<http_status_code> status_{};
    borrowed_http_headers headers_{};
    borrowed_span<set_cookie_options> cookies_{};
    std::optional<http_priority_fields> priority_{};
};

// 显式选择已选响应或跨替换 policy 的元数据编辑能力；不暴露可绕过提交检查的引用。
// 每次修改都检查尚未提交，保存 editor 也不能在提交后修改头。
class context_response_editor final {
public:
    void status(http_status_code status_code);
    void header(std::string_view name, std::string_view value, http_response_header_options options = {});
    void priority(http_priority_fields fields);
    void remove_header(std::string_view name);
    void set_cookie(set_cookie_options options);

private:
    friend class context;
    template <typename host_type>
    friend class request_exchange;
    enum class target_type { selected,
        policy };
    context_response_editor(context& owner, target_type target) noexcept :
        owner_(&owner),
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
    constexpr borrowed_file_path(text_type&& utf8) noexcept(std::is_nothrow_constructible_v<borrowed_text, text_type&&>) :
        text_(std::forward<text_type>(utf8)) {}

    borrowed_file_path(const std::filesystem::path& path) noexcept :
        native_(path.native()) {}

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

// The request exchange context and request_stream share, declared once: the
// connection and worker handles, interim and connection-scoped output, the
// request trailers and streamed multipart content, the cross-replacement
// policy, the streamed response, and the response builders. Each host adds
// respond() and request_body(), whose preconditions differ. Members are
// defined in the library and instantiated there for both hosts.
template <typename host_type>
class request_exchange {
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
    [[nodiscard]] bool push(http_request_head_view request);
    // Connection-scoped ORIGIN (HTTP/2 and HTTP/3, requires TLS) and ALTSVC
    // (HTTP/2) advertisements.
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);
    [[nodiscard]] bool early_data() const noexcept;

    // Connection metadata borrows this request's transport state.
    // conn_info::connection_id() names the connection.
    [[nodiscard]] const conn_info& conn() const& noexcept RUVIA_LIFETIMEBOUND;
    const conn_info& conn() const&& = delete;
    // Copies shared ownership of the state returned by connection_open_.
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept;
    // Closes the entire transport immediately, including every HTTP/2 or
    // HTTP/3 stream; other in-flight requests on it are cancelled too. In a
    // request_stream callback this is a terminal choice: no response is
    // required afterwards, and a later callback exception is still reported.
    // A virtual test connection has no transport to close.
    void abort_connection() noexcept;

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

    // Observes worker shutdown (application::request_stop() closes every connection
    // at once, with no drain phase) and, for a subrequest, explicit
    // cancellation. db(), redis() and run_blocking() bind this
    // token automatically. Explicit operation tokens are combined with it.
    //
    // Cancellation is cooperative: framework waits, including streaming sleep(),
    // observe this token; CPU work and raw Asio waits without it are not preempted.
    // Connection phase timeouts close the socket independently.
    //
    // 流式响应（stream()/stream_sse()）的 token 还观察客户端离开：HTTP/1 请求体
    // 完整消费后对端 FIN/RST 或写失败，HTTP/2 本流 RST_STREAM 或连接终止。
    // 缓冲响应路由不监测读侧终止。request_stream 回调中，非隧道请求被客户端
    // 放弃（HTTP/2、HTTP/3 流被重置）同样触发。
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

    // Read after request_body() reaches EOF to observe the complete terminal
    // section. Trailers never replace initial headers or change route selection.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;

    // Parses request_body() as multipart while it arrives. Decodes no content
    // coding: a coded body is refused with 415 and an invalid
    // Content-Encoding field with 400.
    [[nodiscard]] multipart_reader request_multipart() &;
    multipart_reader request_multipart() && = delete;

    // 显式跨替换策略，只在最终交付/stream commit/升级握手时应用一次，也覆盖
    // error() 与抛出的错误；builder 不继承它。在 next() 前写入的 header 覆盖
    // handler 的同名字段。header/cookie 覆盖同名或同 cookie 身份的响应字段；
    // append 保留重复语义。
    // 警告：显式 policy().status() 也覆盖错误响应；默认不强制任何状态。
    // context::response() 观察的是尚未应用 policy 的选定响应，不是最终 wire head。
    // 提交后任何修改均抛 logic_error。
    [[nodiscard]] context_response_editor policy() & noexcept;
    context_response_editor policy() && = delete;

    // 流式响应的 writer。流头选项应用到待提交的流响应头；默认选项不做任何事。
    // 提交后传入非默认选项抛 logic_error。先构造候选头再整体应用，失败不改动
    // 已暂存的头。缓冲路由没有流式输出，抛 logic_error。request_stream 回调中，
    // 被拒绝的隧道请求用同一个 writer 作答；成功的 CONNECT 或 101 必须改用
    // accept 操作。
    [[nodiscard]] response_stream_writer& stream(response_options head = {}) & RUVIA_LIFETIMEBOUND;
    response_stream_writer& stream(response_options = {}) && = delete;

    // Sends text/plain; charset=UTF-8 and X-Content-Type-Options: nosniff,
    // like text(): a Content-Type or X-Content-Type-Options in `head` is kept.
    [[nodiscard]] response_stream_writer& stream_text(response_options head = {}) & RUVIA_LIFETIMEBOUND;
    response_stream_writer& stream_text(response_options = {}) && = delete;

    // Content-Type is text/event-stream; one in `head` throws
    // std::invalid_argument. Cache-Control defaults to no-cache.
    [[nodiscard]] sse_writer stream_sse(response_options head = {}) &;
    sse_writer stream_sse(response_options = {}) && = delete;

    // Body builders copy their input into response-owned storage, except
    // static_text, which explicitly borrows static bytes without copying, and
    // std::string&&, which transfers existing string allocations. The trailing
    // response_options set status, header lines, cookies and priority of the
    // built response. A builder only builds: hand the result to respond(), or
    // return it from a buffered route handler.
    [[nodiscard]] http_response body(response_options options = {}) const;
    [[nodiscard]] http_response body(outbound_bytes body, response_options options = {}) const;
    [[nodiscard]] http_response body(static_text body, response_options options = {}) const;

    // text/plain; charset=UTF-8 with X-Content-Type-Options: nosniff, like
    // stream_text(); a Content-Type or X-Content-Type-Options in options is kept.
    [[nodiscard]] http_response text(outbound_bytes body, response_options options = {}) const;
    [[nodiscard]] http_response text(static_text body, response_options options = {}) const;

    template <typename t_type>
        requires detail::is_model_json_value<t_type>
    [[nodiscard]] http_response json(const t_type& value, response_options options = {}) const {
        auto serialized = to_json(value);
        return json_serialized(serialized, options);
    }

    [[nodiscard]] http_response html(outbound_bytes body, response_options options = {}) const;
    [[nodiscard]] http_response html(static_text body, response_options options = {}) const;
    // The status comes from `options`; extra.status_ or a Location in
    // extra.headers_ throws std::invalid_argument.
    [[nodiscard]] http_response redirect(redirect_response_options options, response_options extra = {}) const;

    // The status and Content-Type come from `info`; extra.status_ or a
    // Content-Type in extra.headers_ throws std::invalid_argument.
    [[nodiscard]] http_response error(http_error_info_options info, response_options extra = {}) const;
    [[nodiscard]] http_response error(const http_error_info& info, response_options extra = {}) const;

    // File helpers honor If-Range only for matching strong ETags. Last-Modified
    // dates are weak and do not authorize a partial response. Conditional
    // requests, HEAD and single or multipart ranges keep their settled status.
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
    // the returned task. The cold task belongs to the host's request scope.
    //
    // file() opens the file once and the response owns that handle; plain-TCP
    // transfers send from it without resolving the path again. The handle is
    // checked again right before sending: a file changed in place since file()
    // fails a single plain-TCP body before its head leaves and aborts any
    // other transfer before its bytes.
    //
    // head.status_ is the status of a full-body answer (200 when unset);
    // conditional and range answers keep their own status. head is copied
    // before the task is returned.
    [[nodiscard]] task<http_response> file(file_response_options options, response_options head = {});

    // relative_path_ is a decoded file path below the root, such as
    // req().param("*") of a wildcard route; it is never percent-decoded again.
    // '/' and '\\' separate segments. A "." or ".." segment, an empty interior
    // segment, an absolute path, or a NUL is 403 -- rejected, never resolved,
    // whichever parameter it came from -- and an unindexed name 404. A file
    // that is no longer the indexed one is 500 static_file_changed.
    [[nodiscard]] task<http_response> static_file(const static_root& root, static_file_response_options options, response_options head = {});
    // A temporary root would be destroyed before the returned task reads it.
    task<http_response> static_file(const static_root&&, static_file_response_options, response_options = {}) = delete;

private:
    friend host_type;

    request_exchange() = default;

    // The host's context: context itself, or the one a request_stream wraps.
    [[nodiscard]] context& exchange() noexcept;
    [[nodiscard]] const context& exchange() const noexcept;

    [[nodiscard]] static http_response json_serialized(std::string& body, const response_options& options);
};

// Worker resources (worker_state<T>(), run_blocking(), env(), client(), db(),
// redis()) come from worker_services_access. Inside a router dispatch they see
// that router's worker states and clients; executors and clients bind this
// request's operation lifetime and stop_token(). The connection, streaming
// and response-builder members come from request_exchange.
class context final : public worker_services_access<context>, public request_exchange<context> {
private:
    friend class worker_services_access<context>;
    template <typename host_type>
    friend class request_exchange;
    friend class request_stream;
    friend class context_request;
    friend class context_response_editor;
    friend class http_client_response;
    friend struct detail::context_access;
#ifdef RUVIA_ENABLE_REDIS
    friend struct detail::session_access;
#endif
    template <typename t_type>
    friend detail::request_binding_guard<t_type> detail::bind_validated_model(context& context_value, const t_type& model);

    context(const http_request& request, const detail::context_services& services);

    context(const http_request& request, std::string_view route_path, const std::string_view* param_names, const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope, const detail::context_services& services);

public:
    ~context();

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    [[nodiscard]] context_request req() const& noexcept RUVIA_LIFETIMEBOUND {
        return context_request(*this);
    }
    context_request req() const&& = delete;

    // The streamed request content of a stream_body route; throws
    // std::logic_error on a route whose body is buffered.
    [[nodiscard]] body_reader& request_body() & RUVIA_LIFETIMEBOUND;
    body_reader& request_body() && = delete;

    // The exception that failed the current middleware/handler dispatch, or
    // null. Distinct from error(status, code, message) which constructs an
    // error response.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return error_;
    }

#ifdef RUVIA_ENABLE_REDIS
    // Present only while session_middleware is bound for this request.
    [[nodiscard]] ruvia::session session();
#endif

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
    [[nodiscard]] std::string url_for(std::string_view pattern, argument_list<std::string_view> values = {}) const;

    // The session of a negotiated websocket or CONNECT route (controller or
    // router): the server already ran the handshake, so context never accepts
    // one itself; request_stream::accept_websocket()/accept_tunnel() do.
    [[nodiscard]] ::ruvia::websocket& websocket() const;
    [[nodiscard]] http_tunnel& tunnel() const;

    // 明确后处理已 respond 的缓冲响应；尚无响应或已提交时修改抛 logic_error。
    // 字段只属于当前响应，下一次替换不继承；跨替换字段显式写入 policy()。
    [[nodiscard]] context_response_editor edit_response() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::selected);
    }

    // 只读观察已选择的缓冲响应（不包含尚待应用的 policy）；没有响应则为 nullptr。
    [[nodiscard]] const http_response* response() const noexcept;

    [[nodiscard]] bool committed() const noexcept;
    // 将独立响应移入 context；替换丢弃旧响应全部字段。
    // next() 在 respond() 后启动是控制流错误；已提交后不得替换。
    void respond(http_response&& response);

    [[nodiscard]] task<http_response> not_found();

private:
    [[nodiscard]] context& exchange_context() noexcept {
        return *this;
    }
    [[nodiscard]] const context& exchange_context() const noexcept {
        return *this;
    }

    [[nodiscard]] http_response streaming_head();
    // The head a stream commit or upgrade sends; throws once committed.
    [[nodiscard]] http_response& stream_head();
    void stage_stream_head(const response_stream_writer& writer, const response_options& head);

    [[nodiscard]] task<http_response> not_found_task();
    [[nodiscard]] task<dispatch_response> dispatch_task(std::string method, std::string target, std::string body, std::vector<http_header> headers, ::ruvia::stop_token stop);

    [[nodiscard]] std::string& decoded_body() const;
    [[nodiscard]] detail::context_request_storage& request_storage() const;
    [[nodiscard]] detail::worker_services services() const noexcept;
    [[nodiscard]] detail::context_services& runtime_services() noexcept;
    [[nodiscard]] const detail::context_services& runtime_services() const noexcept;
    void store_response(http_response&& response);
    void store_error(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool has_response() const noexcept;

    const http_request& request_;
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
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] detail::context_session_state& session_state() noexcept;
    [[nodiscard]] const detail::context_session_state& session_state() const noexcept;
#endif
    [[nodiscard]] detail::request_bindings& request_bindings() noexcept;
    [[nodiscard]] const detail::request_bindings& request_bindings() const noexcept;

    // Declared last so it closes first, while every request-owned object and its
    // memory resource are still alive.
    mutable detail::operation_scope operation_scope_;
};

enum class readiness_state : std::uint8_t {
    ready,
    unavailable,
};

struct readiness_response_options final {
    readiness_state state_{readiness_state::ready};
    borrowed_text unavailable_reason_{"service is not ready"};
};

// The status is the probe's own (200, or 503 when unavailable); extra.status_
// throws std::invalid_argument.
[[nodiscard]] http_response make_health_response(context& context_value, response_options extra = {});

[[nodiscard]] http_response make_readiness_response(context& context_value, readiness_response_options options = {}, response_options extra = {});

template <typename t_type>
inline request_state_binding<t_type> context::bind_request_state(const t_type& value) {
    return request_bindings().bind_state(value);
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>& context::request_state() const {
    return request_bindings().get_state<t_type>();
}

template <typename t_type>
inline std::optional<t_type> context_request::parse_typed(std::optional<std::string_view> raw, void (*fail)()) {
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
        if (!parsed_value) {
            fail();
        }
        return *parsed_value;
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
[[nodiscard]] task<std::optional<model_type>> parse_optional_request_model(const context& context_value, operation_options operation) {
    if (!request_has_body_media_type<target>(context_value.req().header("Content-Type"))) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_value.req().body_text(std::move(operation));
    co_return parse_and_validate_request_model<target, model_type>(request_body);
}

}  // namespace detail

template <typename t_type>
task<std::optional<t_type>> context_request::json_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    return context_->operation_scope_.attach(detail::parse_optional_request_model<validation_target::json, t_type>(*context_, std::move(operation)));
}

template <typename t_type>
task<std::optional<t_type>> context_request::form_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    return context_->operation_scope_.attach(detail::parse_optional_request_model<validation_target::form, t_type>(*context_, std::move(operation)));
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
        static_assert(always_false<std::integral_constant<validation_target, target>>, "unsupported validator target");
    }
}

template <validation_target target, typename body_type, bool reject_unknown_fields>
task<void> model_validation_middleware<target, body_type, reject_unknown_fields>::invoke(context& c, next& next_value) {
    if constexpr (target == validation_target::json || target == validation_target::form) {
        if (!request_has_body_media_type<target>(c.req().header("Content-Type"))) {
            throw_invalid_content_type<target>();
        }
        const auto request_body = co_await c.req().body_text();
        auto body = parse_and_validate_request_model<target, body_type>(request_body, {.reject_unknown_fields_ = reject_unknown_fields});
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    } else {
        auto body = parse_and_validate_request_model<target, body_type>(validation_target_fields<target>(c).entries());
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    }
}

}  // namespace ruvia::detail
