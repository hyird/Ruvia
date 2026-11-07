#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/model.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"

#include "auth/cookie_signature.h"
#include "context/context_access.h"
#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "http/request_body_loader.h"
#include "http/request_query_values.h"
#include "http/static_path_normalization.h"
#include "http/unsupported_request_content_coding.h"
#include "util/constant_time.h"

namespace ruvia {

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throw_invalid_json_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/json"});
}

[[noreturn]] void throw_invalid_json_body() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid json body"});
}

[[noreturn]] void throw_invalid_form_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/x-www-form-urlencoded"});
}

[[noreturn]] void throw_invalid_query() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid query"});
}

[[noreturn]] void throw_invalid_param() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid route parameter"});
}

}  // namespace detail

namespace {

[[nodiscard]] const request_name_value_list& empty_request_fields() noexcept {
    static const request_name_value_list empty =
        detail::request_name_value_list_access::make(process_resource());
    return empty;
}

// Unencoded names and values borrow the request query string; decoded ones
// live in the request arena. Form decoding keeps an invalid '%' literally.
void ensure_request_query(const context& context_value, const http_request& request,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.query_;
    if (cache) {
        return;
    }
    auto* const arena = context_value.arena();
    auto query = detail::request_name_value_list_access::make(arena);
    (void)visit_url_encoded_pairs(request.query_string(), [&query, arena](std::string_view key, std::string_view value) {
        const auto name = *detail::decode_url_component_in_arena(key, url_decode_mode::form, false, arena);
        const auto decoded_value = *detail::decode_url_component_in_arena(value, url_decode_mode::form, false, arena);
        detail::request_name_value_list_access::push_back(query, http_header_view{name, decoded_value});
        return true;
    });
    cache.emplace(std::move(query), arena);
}

// Counts encoded captures only; decoding validates them once, in ensure_route_params.
[[nodiscard]] std::size_t count_encoded_route_params(const std::string_view* param_values,
    std::size_t param_count, detail::context_request_storage& request_storage) {
    if (request_storage.route_params_unencoded_) {
        return 0;
    }
    std::size_t encoded_count = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        encoded_count += has_url_encoding(param_values[i], url_decode_mode::percent);
    }
    if (encoded_count == 0) {
        request_storage.route_params_unencoded_ = true;
    }
    return encoded_count;
}

// Route names and unencoded captures borrow stable route/request storage; a
// "*" capture rejects escapes revealing a path separator, as static paths do.
void ensure_route_params(const context& context_value, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.route_params_;
    if (cache) {
        return;
    }
    // A capture that failed to decode keeps failing without another decode.
    if (request_storage.route_params_invalid_) {
        detail::throw_invalid_param();
    }
    auto* const arena = context_value.arena();
    auto params = detail::request_name_value_list_access::make(arena);
    detail::request_name_value_list_access::reserve(params, param_count);
    for (std::size_t i = 0; i < param_count; ++i) {
        const auto value = detail::decode_url_component_in_arena(
            param_values[i], url_decode_mode::percent, param_names[i] == "*", arena);
        if (!value) {
            request_storage.route_params_invalid_ = true;
            detail::throw_invalid_param();
        }
        detail::request_name_value_list_access::push_back(params, http_header_view{param_names[i], *value});
    }
    cache.emplace(std::move(params));
}

[[nodiscard]] std::string_view negotiation_header_name(context_request::negotiable_field field) noexcept {
    switch (field) {
        case context_request::negotiable_field::language:
            return "Accept-Language";
        case context_request::negotiable_field::encoding:
            return "Accept-Encoding";
        case context_request::negotiable_field::charset:
            return "Accept-Charset";
        case context_request::negotiable_field::media_type:
            break;
    }
    return "Accept";
}

[[nodiscard]] multipart_boundary get_multipart_boundary(const http_request& request) {
    const auto boundary = parse_multipart_boundary(
        request.header("Content-Type").value_or(std::string_view{}));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.not_applicable() != nullptr) {
        throw http_error({.status_ = http_status::unsupported_media_type,
            .code_ = "unsupported_media_type",
            .message_ = "request body must be multipart/form-data"});
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocol_error();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

// Decoded bytes a request body may produce on the event loop. A body that
// decodes past it (a large body, or a few-KiB compression bomb) is decoded
// again on the worker's bounded blocking pool. Without a pool, or when the pool
// rejects the job, it is decoded inline: the handler requires the content, and
// the route's decoded-size cap still bounds the work.
inline constexpr std::size_t inline_request_decode_bytes = 64 * 1024;

[[nodiscard]] task<std::variant<std::pmr::string, http_content_decode_error>> decode_request_content(
    const worker_handle& worker_value, const stop_token& stop_token_value,
    std::span<const http_content_coding> codings, std::string_view raw,
    http_content_decode_options options) {
    if (worker_value.has_blocking_pool() && options.max_decoded_bytes_ > inline_request_decode_bytes) {
        auto inline_result = decode_http_content(codings, raw,
            {.max_decoded_bytes_ = inline_request_decode_bytes, .resource_ = options.resource_});
        const auto* failure = std::get_if<http_content_decode_error>(&inline_result);
        if (failure == nullptr || *failure != http_content_decode_error::decoded_size_exceeded) {
            co_return inline_result;
        }
        // The job carries only its own copies; request memory stays on this worker.
        std::pmr::vector<http_content_coding> owned_codings(codings.begin(), codings.end(), process_resource());
        std::pmr::string owned_raw(raw, process_resource());
        auto offloaded = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [owned_codings = std::move(owned_codings), owned_raw = std::move(owned_raw),
                limit = options.max_decoded_bytes_]() {
                return decode_http_content(owned_codings, owned_raw,
                    {.max_decoded_bytes_ = limit, .resource_ = process_resource()});
            });
        // A codec failure on the pool is rethrown here; pool saturation or
        // shutdown throws blocking_operation_rejected (mapped to 503).
        auto result_value = std::move(offloaded).value();
        if (const auto* content = std::get_if<std::pmr::string>(&result_value)) {
            co_return std::pmr::string(*content, options.resource_);
        }
        co_return std::get<http_content_decode_error>(result_value);
    }
    co_return decode_http_content(codings, raw, options);
}

// Rejects an invalid or unsupported Content-Encoding; the result owns the codings span.
[[nodiscard]] http_content_coding_field_result require_decodable_request_coding(
    const http_request& request, detail::unsupported_request_content_coding::read_type read) {
    auto parsed_coding = request_content_coding(request);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(
            invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
    }
    if (parsed_coding.unsupported() != nullptr) {
        throw detail::unsupported_request_content_coding(read);
    }
    return parsed_coding;
}

task<std::string_view> read_request_text(const context& context_value) {
    if (const auto decoded = detail::context_access::decoded_body(context_value)) {
        co_return *decoded;
    }

    const auto& body_source = detail::context_access::request_body_source(context_value);
    const auto& request = detail::context_access::request(context_value);
    std::string_view raw;
    if (const auto* lazy = body_source.lazy()) {
        raw = co_await lazy->loader().read_all();
    } else if (body_source.streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = as_chars(request.body_bytes());
    }
    if (detail::context_access::body_identity(context_value)) {
        co_return raw;
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto& services = detail::context_access::services(context_value);
    auto* const decode_resource = services.inbound_buffer_pool() != nullptr
                                      ? services.inbound_buffer_pool()
                                      : context_value.pool();
    const auto parsed_coding = require_decodable_request_coding(
        request, detail::unsupported_request_content_coding::read_type::decoded);
    const auto codings = parsed_coding.codings();
    if (codings.empty()) {
        detail::context_access::mark_body_identity(context_value);
        co_return raw;
    }
    auto decode_result = co_await decode_request_content(services.worker(), context_value.stop_token(), codings, raw,
        {.max_decoded_bytes_ = services.max_decoded_request_body_bytes(),
            .resource_ = decode_resource});
    auto* decoded_content = std::get_if<std::pmr::string>(&decode_result);
    if (decoded_content == nullptr) {
        throw http_request_content_decode_error(std::get<http_content_decode_error>(decode_result));
    }
    co_return detail::context_access::store_decoded_body(context_value, std::move(*decoded_content));
}

task<std::span<const std::byte>> read_request_bytes(const context& context_value) {
    const auto body = co_await context_value.req().text();
    co_return std::as_bytes(std::span(body));
}

task<std::pmr::vector<multipart_part>> read_request_multipart(const context& context_value) {
    const auto boundary = get_multipart_boundary(detail::context_access::request(context_value));
    const auto request_body = co_await context_value.req().text();
    co_return parse_multipart_body(request_body, {.boundary_ = boundary, .resource_ = context_value.arena()});
}

task<void> discard_request_body(const context& context_value) {
    const auto& body_source = detail::context_access::request_body_source(context_value);
    if (const auto* lazy = body_source.lazy()) {
        co_await lazy->loader().discard();
        co_return;
    }
    if (const auto* streaming = body_source.streaming()) {
        while (co_await streaming->reader().read()) {
        }
    }
}

}  // namespace

std::string_view context_request::method() const noexcept {
    return context_->request_.method();
}

http_known_method context_request::known_method() const noexcept {
    return context_->request_.known_method();
}

std::string_view context_request::path() const noexcept {
    return context_->request_.path();
}

std::string_view context_request::scheme() const noexcept {
    return context_->request_.scheme();
}

std::string_view context_request::authority() const noexcept {
    return context_->request_.authority();
}

std::string_view context_request::target() const noexcept {
    return context_->request_.target();
}

http_protocol_version context_request::protocol_version() const noexcept {
    return context_->request_.protocol_version();
}

http_request_target_form context_request::target_form() const noexcept {
    return context_->request_.target_form();
}

std::string_view context_request::route_path() const noexcept {
    return context_->route_path_;
}

http_priority context_request::priority() const noexcept {
    if (const auto* update = context_->services().request_priority_update(); update != nullptr && *update) {
        return **update;
    }
    const auto parsed_value = parse_http_priority(context_->request_.headers());
    return (parsed_value.index() == 0) ? std::get<0>(parsed_value).request_priority() : http_priority{};
}

void context_response_editor::priority(http_priority_fields fields_value) {
    std::array<char, 12> value{};
    const auto encoded = encode_http_priority(value, fields_value);
    if ((encoded.index() != 0)) {
        throw std::invalid_argument("invalid HTTP priority parameters");
    }
    header("Priority", std::string_view(value.data(), std::get<0>(encoded)));
}

std::optional<std::string_view> context_request::header(std::string_view name) const {
    return context_->request_.header(name);
}

std::span<const http_header> context_request::trailers() const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->fields()
                                                              : std::span<const http_header>{};
}

std::optional<std::string_view> context_request::trailer(std::string_view name) const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->field(name)
                                                              : std::nullopt;
}

const request_name_value_list& context_request::header_fields() const {
    auto& cache = context_->request_storage().headers_;
    if (!cache) {
        cache.emplace(detail::request_name_value_list_access::borrow_headers(context_->request_.headers()));
    }
    return *cache;
}

const request_name_value_list& context_request::query_fields() const {
    if (context_->request_.query_string().empty()) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->fields();
}

std::optional<std::string_view> context_request::query(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->fields().get(name);
}

std::span<const std::string_view> context_request::queries(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return {};
    }
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->values().values(name);
}

std::optional<std::string_view> context_request::cookie(std::string_view name) const {
    if (const auto& cache = context_->request_storage().cookies_) {
        return cache->get(name);
    }
    return context_->request_.cookie(name);
}

const request_name_value_list& context_request::cookie_fields() const {
    auto& cache = context_->request_storage().cookies_;
    if (cache) {
        return *cache;
    }
    // Pool-backed growth returns replaced descriptor blocks immediately.
    // Keep the candidate private until every Cookie field has been visited.
    std::optional<request_name_value_list> cookies;
    for (const auto& header_field : context_->request_.headers()) {
        if (header_field.name().size() != 6U ||
            !http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
            continue;
        }
        if (!cookies) {
            cookies.emplace(detail::request_name_value_list_access::make(context_->pool()));
        }
        http_visit_cookie_pairs(header_field.value(),
            [&cookies](std::string_view key, std::string_view value) {
                detail::request_name_value_list_access::push_back(
                    *cookies, http_header_view{key, value});
                return true;
            });
    }
    if (!cookies) {
        return empty_request_fields();
    }
    cache.emplace(std::move(*cookies));
    return *cache;
}

const request_name_value_list& context_request::param_fields() const {
    if (context_->param_count_ == 0) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_route_params(
        *context_, context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return *storage.route_params_;
}

std::optional<std::string_view> context_request::param(std::string_view name) const {
    if (context_->param_count_ == 0) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    if (storage.route_params_) {
        return storage.route_params_->get(name);
    }
    if (count_encoded_route_params(context_->param_values_, context_->param_count_, storage) == 0) {
        for (std::size_t i = context_->param_count_; i > 0; --i) {
            if (context_->param_names_[i - 1] == name) {
                return context_->param_values_[i - 1];
            }
        }
        return std::nullopt;
    }
    ensure_route_params(
        *context_, context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return storage.route_params_->get(name);
}

std::optional<std::string_view> context_request::negotiate(
    negotiable_field field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const bool media_type = field == negotiable_field::media_type;
    const bool encoding = field == negotiable_field::encoding;
    const auto header_name = negotiation_header_name(field);
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == negotiable_field::language
                                ? http_accept_token_match_mode::language_prefix
                                : http_accept_token_match_mode::exact;

    static constexpr std::size_t inline_value_capacity = 8;
    std::array<std::string_view, inline_value_capacity> inline_values{};
    std::size_t matching_count = 0;
    bool saw_header = false;
    const auto headers = context_->request_.headers();
    for (const auto& header_field : headers) {
        if (!http_ascii_equals_ignore_case(header_field.name(), header_name)) {
            continue;
        }
        saw_header = true;
        if (header_field.value().empty()) {
            continue;
        }
        if (matching_count < inline_values.size()) {
            inline_values[matching_count] = header_field.value();
        }
        ++matching_count;
    }
    if (!saw_header) {
        return supported.front();
    }
    if (matching_count == 0 && !encoding) {
        return std::nullopt;
    }
    std::optional<std::string_view> best;
    int best_quality = 0;
    const bool use_inline_values = matching_count <= inline_values.size();
    for (const auto offered : supported) {
        http_accept_match match;
        http_accepted_encoding_quality encoding_quality;
        const auto accumulate = [&](std::string_view field_value) noexcept {
            if (media_type) {
                match.update_media_type(field_value, offered);
            } else if (encoding) {
                encoding_quality.update(field_value, offered);
            } else {
                match.update_token(field_value, offered, token_mode);
            }
        };
        if (use_inline_values) {
            for (std::size_t i = 0; i < matching_count; ++i) {
                accumulate(inline_values[i]);
            }
        } else {
            for (const auto& header_field : headers) {
                if (http_ascii_equals_ignore_case(header_field.name(), header_name) &&
                    !header_field.value().empty()) {
                    accumulate(header_field.value());
                }
            }
        }
        const auto quality = encoding ? encoding_quality.quality(http_ascii_equals_ignore_case(offered, "identity"))
                                      : match.quality();
        if (quality == 0) {
            continue;
        }
        // Strictly greater, so `supported` order breaks the client's ties and
        // reads as the server's own preference.
        if (quality > best_quality) {
            best_quality = quality;
            best = offered;
        }
    }

    // An empty Accept-Encoding accepts identity; other empty negotiation fields
    // match no representation.
    return best;
}

task<std::string_view> context_request::text() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_text(*context_));
}

task<std::span<const std::byte>> context_request::bytes() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_bytes(*context_));
}

std::optional<std::string_view> context_request::signed_cookie(
    signed_cookie_lookup_options options) const {
    const auto name = options.name_.view();
    const auto secret = options.secret_.view();
    const auto stored = cookie(name);
    if (!stored.has_value() || stored->size() <= detail::cookie_signature_size) {
        return std::nullopt;
    }
    const auto value_size = stored->size() - detail::cookie_signature_size - 1;
    if ((*stored)[value_size] != '.') {
        return std::nullopt;
    }
    const auto value = stored->substr(0, value_size);
    const auto signature = stored->substr(value_size + 1);
    char expected[detail::cookie_signature_size];
    detail::write_cookie_signature(expected, secret, name, value, context_->pool());
    if (!detail::constant_time_bytes_equal(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

task<std::pmr::vector<multipart_part>> context_request::multipart() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_multipart(*context_));
}

task<void> context_request::discard_body() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, discard_request_body(*context_));
}

body_reader& context_request::body_reader() const {
    const auto* streaming = context_->request_body_source().streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

multipart_reader context_request::multipart_reader() const {
    const auto boundary = get_multipart_boundary(context_->request_);
    // The reader parses raw body bytes as they arrive and decodes no content
    // coding, so only an identity body is accepted.
    const auto parsed_coding = require_decodable_request_coding(
        context_->request_, detail::unsupported_request_content_coding::read_type::streaming);
    if (std::ranges::any_of(parsed_coding.codings(),
            [](http_content_coding coding) { return coding != http_content_coding::identity; })) {
        throw detail::unsupported_request_content_coding(
            detail::unsupported_request_content_coding::read_type::streaming);
    }
    return ::ruvia::multipart_reader(body_reader(), {.boundary_ = boundary, .resource_ = context_->pool()});
}
}  // namespace ruvia
