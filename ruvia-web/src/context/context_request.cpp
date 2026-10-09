#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_cookie_fields.h"
#include "ruvia/http/http_request_content_decoding.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/model_json.h"

#include "auth/cookie_signature.h"
#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "http/request_body_loader.h"
#include "http/request_field_parsing.h"
#include "http/request_query_values.h"
#include "http/unsupported_request_content_coding.h"

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

[[noreturn]] void throw_invalid_form_body() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid form body"});
}

[[noreturn]] void throw_invalid_query() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid query"});
}

[[noreturn]] void throw_invalid_param() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid route parameter"});
}

[[noreturn]] void throw_invalid_header() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid request header"});
}

[[noreturn]] void throw_invalid_cookie() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid cookie"});
}

}  // namespace detail

const request_name_value_list& context::request_headers() const {
    auto& cache = request_storage().headers_;
    if (!cache) {
        cache.emplace(detail::request_name_value_list_access::borrow_headers(request_.headers()));
    }
    return *cache;
}

std::optional<std::string_view> context::request_header(std::string_view name) const {
    return request_.header(name);
}

void context::ensure_request_query() const {
    auto& cache = request_storage().query_;
    if (cache) {
        return;
    }
    if (request_storage_->query_invalid_) {
        detail::throw_invalid_query();
    }
    // Percent-encoding is validated while decoding each component. Unencoded
    // names and values borrow the request query string instead of copying.
    std::pmr::vector<std::pmr::string> storage(arena());
    // Publish views only after capacity is fixed: moving a short string during
    // vector growth invalidates views into its inline buffer. Count decoded
    // owners exactly so unencoded fields do not reserve unused string storage.
    std::size_t decoded_component_count = 0;
    (void)visit_url_encoded_pairs(request_.query_string(),
        [&decoded_component_count](std::string_view name, std::string_view value) {
            decoded_component_count += has_url_encoding(name, url_decode_mode::form);
            decoded_component_count += has_url_encoding(value, url_decode_mode::form);
        });
    storage.reserve(decoded_component_count);
    auto query = detail::request_name_value_list_access::make(arena());
    bool valid = true;
    const bool completed = visit_url_encoded_pairs(request_.query_string(),
        [&storage, &query, &valid](std::string_view key, std::string_view value) {
            const auto name =
                detail::borrow_or_decode(storage, key, url_decode_mode::form);
            const auto decoded_value =
                detail::borrow_or_decode(storage, value, url_decode_mode::form);
            if (!name || !decoded_value) {
                valid = false;
                return false;
            }
            detail::request_name_value_list_access::push_back(
                query, detail::request_name_value_view_access::make(*name, *decoded_value));
            return true;
        });
    if (!completed || !valid) {
        request_storage_->query_invalid_ = true;
        detail::throw_invalid_query();
    }

    struct query_build final {
        std::size_t first_index_;
        std::size_t begin_;
        std::size_t end_;
    };

    const auto order = detail::sorted_field_order(query, arena());
    std::pmr::vector<query_build> builds(arena());
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto first_index = order[offset];
        const auto name = query[first_index].name();
        do {
            ++offset;
        } while (offset < order.size() && query[order[offset]].name() == name);
        builds.push_back(query_build{.first_index_ = first_index, .begin_ = begin, .end_ = offset});
    }
    std::ranges::sort(builds, [](const query_build& left, const query_build& right) noexcept {
        return left.first_index_ < right.first_index_;
    });

    detail::request_query_values groups{arena()};
    groups.reserve(builds.size());
    for (const auto& build : builds) {
        // A duplicated query name resolves to its LAST value, matching every other
        // duplicate-resolution path: context::request_query(name), http_request::query,
        // and request_name_value_list::get() all take the last occurrence. Keep the
        // public field list duplicate-preserving so model binding can still reject
        // ambiguity; this grouped index only backs request_queries(name).
        auto& group = groups.append(query[build.first_index_].name());
        for (std::size_t i = build.begin_; i < build.end_; ++i) {
            group.add(query[order[i]].value());
        }
    }

    cache.emplace(std::move(storage), std::move(query), std::move(groups));
}

const request_name_value_list& context::request_query() const {
    ensure_request_query();
    return request_storage_->query_->fields();
}

std::optional<std::string_view> context::request_query(std::string_view name) const {
    ensure_request_query();
    return request_storage_->query_->fields().get(name);
}

const detail::request_query_values& context::request_queries() const {
    ensure_request_query();
    return request_storage_->query_->values();
}

std::optional<std::string_view> context::request_cookie(std::string_view name) const {
    return request_.cookie(name);
}

const request_name_value_list& context::request_cookies() const {
    auto& cache = request_storage().cookies_;
    if (!cache) {
        auto cookies = detail::request_name_value_list_access::make(arena());
        const auto headers = request_.headers();
        if (request_.header("Cookie").has_value()) {
            detail::request_name_value_list_access::reserve(
                cookies, detail::bounded_field_reserve(8));
        }
        for (const auto& header : headers) {
            if (!http_ascii_equals_ignore_case(header.name(), "Cookie")) {
                continue;
            }
            http_visit_cookie_pairs(header.value(), [&cookies](std::string_view key,
                                                        std::string_view value) {
                detail::request_name_value_list_access::push_back(
                    cookies, detail::request_name_value_view_access::make(key, value));
                return true;
            });
        }
        cache.emplace(std::move(cookies));
    }
    return *cache;
}

void context::ensure_route_params() const {
    auto& cache = request_storage().route_params_;
    if (cache) {
        return;
    }
    if (request_storage_->route_params_invalid_) {
        detail::throw_invalid_param();
    }
    // Route names and unencoded captures already borrow stable route/request
    // storage. Own only decoded values. Invalid percent-escapes fail in decode
    // rather than in a separate pre-scan of the same captures.
    std::pmr::vector<std::pmr::string> storage(arena());
    auto params = detail::request_name_value_list_access::make(arena());
    storage.reserve(param_count_);
    detail::request_name_value_list_access::reserve(params, param_count_);
    for (std::size_t i = 0; i < param_count_; ++i) {
        auto value = param_values_[i];
        if (has_url_encoding(value, url_decode_mode::percent)) {
            auto decoded = decode_url_component(
                value, {.mode_ = url_decode_mode::percent, .resource_ = arena()});
            if (!decoded) {
                request_storage_->route_params_invalid_ = true;
                detail::throw_invalid_param();
            }
            auto& owned = storage.emplace_back(std::move(*decoded));
            value = detail::stored_string_view(owned);
        }
        detail::request_name_value_list_access::push_back(
            params, detail::request_name_value_view_access::make(param_names_[i], value));
    }
    cache.emplace(std::move(storage), std::move(params));
}

const request_name_value_list& context::route_params() const {
    ensure_route_params();
    return request_storage_->route_params_->fields_;
}

std::optional<std::string_view> context::route_param(std::string_view name) const {
    ensure_route_params();
    return request_storage_->route_params_->fields_.get(name);
}

bool context::request_accepts(std::string_view media_type) const noexcept {
    // RFC 9110 5.3: multiple Accept field lines are equivalent to a single value
    // comma-joining them. request_known_header returns only one stored slot, so a
    // client that sent Accept across several lines had all but one ignored. Fold
    // every Accept line into one best-match accumulator (equivalent to the joined
    // value, and correct for a q=0 exclusion spread across lines) without
    // allocating to concatenate.
    if (!request_.header("Accept").has_value()) {
        return true;
    }
    http_accept_match match;
    bool saw_accept = false;
    const auto headers = request_.headers();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Accept")) {
            continue;
        }
        saw_accept = true;
        if (!header.value().empty()) {
            match.update_media_type(header.value(), media_type);
        }
    }
    // Only absence means no preference. A present but empty Accept field is an
    // empty media-range list and therefore matches no representation.
    if (!saw_accept) {
        return true;
    }
    return match.matched();
}

namespace {

[[nodiscard]] std::string_view negotiation_header_name(context_request::negotiable_type field) noexcept {
    switch (field) {
        case context_request::negotiable_type::language:
            return "Accept-Language";
        case context_request::negotiable_type::encoding:
            return "Accept-Encoding";
        case context_request::negotiable_type::charset:
            return "Accept-Charset";
        case context_request::negotiable_type::media_type:
            break;
    }
    return "Accept";
}

}  // namespace

std::optional<std::string_view> context::request_negotiate(
    context_request::negotiable_type field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const auto header_name = negotiation_header_name(field);
    const bool media_type = field == context_request::negotiable_type::media_type;
    const bool encoding = field == context_request::negotiable_type::encoding;
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == context_request::negotiable_type::language
                                ? http_accept_token_match_mode::language_prefix
                                : http_accept_token_match_mode::exact;

    std::array<std::string_view, max_http_header_fields> field_values{};
    std::size_t field_value_count = 0;
    bool saw_field = false;
    const auto headers = request_.headers();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), header_name)) {
            continue;
        }
        saw_field = true;
        if (header.value().empty() || field_value_count == field_values.size()) {
            continue;
        }
        field_values[field_value_count++] = header.value();
    }

    if (!saw_field) {
        return supported.front();
    }

    std::optional<std::string_view> best;
    int best_quality = 0;
    for (const auto offered : supported) {
        http_accept_match match;
        http_accepted_encoding_quality encoding_quality;
        for (std::size_t i = 0; i < field_value_count; ++i) {
            if (media_type) {
                match.update_media_type(field_values[i], offered);
            } else if (encoding) {
                encoding_quality.update(field_values[i], offered);
            } else {
                match.update_token(field_values[i], offered, token_mode);
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

task<std::string_view> context::request_body() const {
    if (body_decoded_) {
        const auto& decoded = *request_storage_->decoded_body_;
        co_return std::string_view(decoded);
    }

    std::string_view raw;
    if (const auto* lazy = request_body_source().lazy()) {
        raw = co_await lazy->loader().read_all();
    } else if (request_body_source().streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = as_chars(request_.body_bytes());
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    auto* const decode_resource = services().inbound_buffer_pool() != nullptr ? services().inbound_buffer_pool() : pool();
    const auto parsed_coding = request_content_coding(request_, decode_resource);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(invalid->status(), "invalid request Content-Encoding");
    }
    if (const auto* unsupported = parsed_coding.unsupported()) {
        throw detail::unsupported_request_content_coding(*unsupported);
    }
    const auto codings = parsed_coding.codings();
    if (codings.empty()) {
        co_return raw;
    }
    auto decode_result = decode_http_request_content(codings, raw,
        {.max_decoded_bytes_ = services().max_decoded_body_bytes(), .resource_ = decode_resource});
    auto* decoded_content = decode_result.decoded();
    if (decoded_content == nullptr) {
        if (const auto* failure = decode_result.protocol_failure()) {
            throw failure->protocol_error();
        }
        if (decode_result.decoder_failure() != nullptr) {
            throw std::runtime_error("request content decoder failed");
        }
        throw std::logic_error("unexpected request content decode result");
    }
    auto& decoded = decoded_body();
    decoded = std::move(*decoded_content).take_bytes();
    body_decoded_ = true;
    co_return std::string_view(decoded);
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
    detail::write_cookie_signature(expected, secret, name, value);
    if (!detail::cookie_signature_equals(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

bool context::request_content_type_matches(std::string_view expected) const noexcept {
    return detail::content_type_matches(request_.header("Content-Type").value_or(std::string_view{}),
        expected);
}

task<std::pmr::vector<multipart_part>> context::request_multipart() const {
    const auto boundary = get_multipart_boundary();
    const auto request_body = co_await this->request_body();
    auto parsed_value = parse_multipart_body(request_body, {.boundary_ = boundary, .resource_ = arena()});
    if (const auto* failure = parsed_value.failure()) {
        throw failure->protocol_error();
    }
    auto* body = parsed_value.body();
    if (body == nullptr) {
        throw std::logic_error("unexpected multipart body parse result");
    }
    co_return std::move(*body).take_parts();
}

task<void> context::request_discard_body() const {
    if (const auto* lazy = request_body_source().lazy()) {
        co_await lazy->loader().discard();
        co_return;
    }
    if (const auto* streaming = request_body_source().streaming()) {
        while (co_await streaming->reader().read()) {
        }
    }
}

body_reader& context::request_body_reader() const {
    const auto* streaming = request_body_source().streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

multipart_reader context::request_multipart_reader() const {
    return multipart_reader(
        request_body_reader(), {.boundary_ = get_multipart_boundary(), .resource_ = pool()});
}

multipart_boundary context::get_multipart_boundary() const {
    const auto boundary = parse_multipart_boundary(
        request_.header("Content-Type").value_or(std::string_view{}));
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

}  // namespace ruvia
