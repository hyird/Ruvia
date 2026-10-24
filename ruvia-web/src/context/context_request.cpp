#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/model.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"

#include "auth/cookie_signature.h"
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

namespace {

// Storage capacity is fixed by ensure_request_query before any view escapes.
[[nodiscard]] std::optional<std::string_view> borrow_or_decode_query_component(
    std::pmr::vector<std::pmr::string>& storage, std::string_view input) {
    if (!has_url_encoding(input, url_decode_mode::form)) {
        return input;
    }
    auto decoded = decode_url_component(input,
        {.mode_ = url_decode_mode::form, .resource_ = storage.get_allocator().resource()});
    if (!decoded) {
        return std::nullopt;
    }
    return storage.emplace_back(std::move(*decoded));
}
// Storage capacity is fixed by ensure_request_query before any view escapes.
void ensure_request_query(const context& context_value, const http_request& request,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.query_;
    if (cache) {
        return;
    }
    if (request_storage.query_invalid_) {
        detail::throw_invalid_query();
    }
    // Percent-encoding is validated while decoding each component. Unencoded
    // names and values borrow the request query string instead of copying.
    std::pmr::vector<std::pmr::string> storage(context_value.arena());
    // Publish views only after capacity is fixed: moving a short string during
    // vector growth invalidates views into its inline buffer. Count decoded
    // owners exactly so unencoded fields do not reserve unused string storage.
    std::size_t decoded_component_count = 0;
    std::size_t pair_count = 0;
    (void)visit_url_encoded_pairs(request.query_string(),
        [&decoded_component_count, &pair_count](std::string_view name, std::string_view value) {
            ++pair_count;
            decoded_component_count += has_url_encoding(name, url_decode_mode::form);
            decoded_component_count += has_url_encoding(value, url_decode_mode::form);
        });
    auto query = detail::request_name_value_list_access::make(context_value.arena());
    detail::request_name_value_list_access::reserve(query, pair_count);
    if (decoded_component_count == 0) {
        (void)visit_url_encoded_pairs(
            request.query_string(), [&query](std::string_view key, std::string_view value) {
                detail::request_name_value_list_access::push_back(query, http_header_view{key, value});
                return true;
            });
        cache.emplace(std::move(storage), std::move(query));
        return;
    }
    storage.reserve(decoded_component_count);
    bool valid = true;
    const bool completed = visit_url_encoded_pairs(request.query_string(),
        [&storage, &query, &valid](std::string_view key, std::string_view value) {
            const auto name = borrow_or_decode_query_component(storage, key);
            const auto decoded_value = borrow_or_decode_query_component(storage, value);
            if (!name || !decoded_value) {
                valid = false;
                return false;
            }
            detail::request_name_value_list_access::push_back(
                query, http_header_view{*name, *decoded_value});
            return true;
        });
    if (!completed || !valid) {
        request_storage.query_invalid_ = true;
        detail::throw_invalid_query();
    }

    cache.emplace(std::move(storage), std::move(query));
}

void ensure_route_params(const context& context_value, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.route_params_;
    if (cache) {
        return;
    }
    if (request_storage.route_params_invalid_) {
        detail::throw_invalid_param();
    }
    // Route names and unencoded captures already borrow stable route/request
    // storage. Own only decoded values. Invalid percent-escapes fail in decode
    // rather than in a separate pre-scan of the same captures.
    std::pmr::vector<std::pmr::string> storage(context_value.arena());
    auto params = detail::request_name_value_list_access::make(context_value.arena());
    storage.reserve(param_count);
    detail::request_name_value_list_access::reserve(params, param_count);
    for (std::size_t i = 0; i < param_count; ++i) {
        auto value = param_values[i];
        if (has_url_encoding(value, url_decode_mode::percent)) {
            // The wildcard capture is a path tail handed to static_file as is,
            // so it decodes under the same request-path rule as the document
            // root: no escape may reveal a separator the router never split on.
            auto decoded = param_names[i] == "*"
                               ? detail::decode_request_path(value, context_value.arena())
                               : decode_url_component(
                                     value, {.mode_ = url_decode_mode::percent, .resource_ = context_value.arena()});
            if (!decoded) {
                request_storage.route_params_invalid_ = true;
                detail::throw_invalid_param();
            }
            value = storage.emplace_back(std::move(*decoded));
        }
        detail::request_name_value_list_access::push_back(params, http_header_view{param_names[i], value});
    }
    cache.emplace(std::move(storage), std::move(params));
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

void context::priority(http_priority_fields fields_value) {
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
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->fields();
}

std::optional<std::string_view> context_request::query(std::string_view name) const {
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->fields().get(name);
}

std::span<const std::string_view> context_request::queries(std::string_view name) const {
    auto& storage = context_->request_storage();
    ensure_request_query(*context_, context_->request_, storage);
    return storage.query_->values().values(name);
}

std::optional<std::string_view> context_request::cookie(std::string_view name) const {
    return context_->request_.cookie(name);
}

const request_name_value_list& context_request::cookie_fields() const {
    auto& cache = context_->request_storage().cookies_;
    if (!cache) {
        auto cookies = detail::request_name_value_list_access::make(context_->arena());
        if (context_->request_.header("Cookie").has_value()) {
            detail::request_name_value_list_access::reserve(cookies, 8);
            for (const auto& header_field : context_->request_.headers()) {
                if (!http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
                    continue;
                }
                http_visit_cookie_pairs(
                    header_field.value(), [&cookies](std::string_view key, std::string_view value) {
                        detail::request_name_value_list_access::push_back(
                            cookies, http_header_view{key, value});
                        return true;
                    });
            }
        }
        cache.emplace(std::move(cookies));
    }
    return *cache;
}

const request_name_value_list& context_request::param_fields() const {
    auto& storage = context_->request_storage();
    ensure_route_params(*context_, context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return storage.route_params_->fields_;
}

std::optional<std::string_view> context_request::param(std::string_view name) const {
    auto& storage = context_->request_storage();
    ensure_route_params(*context_, context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return storage.route_params_->fields_.get(name);
}

bool context_request::accepts(std::string_view media_type) const noexcept {
    // RFC 9110 5.3: multiple Accept field lines are equivalent to a single value
    // comma-joining them. request_known_header returns only one stored slot, so a
    // client that sent Accept across several lines had all but one ignored. Fold
    // every Accept line into one best-match accumulator (equivalent to the joined
    // value, and correct for a q=0 exclusion spread across lines) without
    // allocating to concatenate.
    if (!context_->request_.header("Accept").has_value()) {
        return true;
    }
    http_accept_match match;
    bool saw_accept = false;
    const auto headers = context_->request_.headers();
    for (const auto& header_field : headers) {
        if (!http_ascii_equals_ignore_case(header_field.name(), "Accept")) {
            continue;
        }
        saw_accept = true;
        if (!header_field.value().empty()) {
            match.update_media_type(header_field.value(), media_type);
        }
    }
    // Only absence means no preference. A present but empty Accept field is an
    // empty media-range list and therefore matches no representation.
    if (!saw_accept) {
        return true;
    }
    return match.matched();
}

std::optional<std::string_view> context_request::negotiate(
    negotiable_field field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const auto header_name = negotiation_header_name(field);
    if (!context_->request_.header(header_name).has_value()) {
        return supported.front();
    }
    const bool media_type = field == negotiable_field::media_type;
    const bool encoding = field == negotiable_field::encoding;
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == negotiable_field::language
                                ? http_accept_token_match_mode::language_prefix
                                : http_accept_token_match_mode::exact;

    bool saw_field = false;
    const auto headers = context_->request_.headers();
    for (const auto& header_field : headers) {
        if (http_ascii_equals_ignore_case(header_field.name(), header_name)) {
            saw_field = true;
            break;
        }
    }

    if (!saw_field) {
        return supported.front();
    }

    std::optional<std::string_view> best;
    int best_quality = 0;
    for (const auto offered : supported) {
        http_accept_match match;
        http_accepted_encoding_quality encoding_quality;
        for (const auto& header_field : headers) {
            if (!http_ascii_equals_ignore_case(header_field.name(), header_name) ||
                header_field.value().empty()) {
                continue;
            }
            if (media_type) {
                match.update_media_type(header_field.value(), offered);
            } else if (encoding) {
                encoding_quality.update(header_field.value(), offered);
            } else {
                match.update_token(header_field.value(), offered, token_mode);
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
    const auto run = [](const context* context_value) -> task<std::string_view> {
        if (context_value->body_decoded_) {
            const auto& decoded = *context_value->request_storage_->decoded_body_;
            co_return std::string_view(decoded);
        }

        std::string_view raw;
        if (const auto* lazy = context_value->request_body_source().lazy()) {
            raw = co_await lazy->loader().read_all();
        } else if (context_value->request_body_source().streaming() != nullptr) {
            throw std::logic_error("streaming request body cannot be buffered");
        } else {
            raw = as_chars(context_value->request_.body_bytes());
        }
        if (context_value->body_identity_) {
            co_return raw;
        }

        // Transparently decode a request body whose Content-Encoding we understand,
        // so handlers always see the decoded representation (RFC 9110 §8.4).
        auto* const decode_resource = context_value->services().inbound_buffer_pool() != nullptr
                                          ? context_value->services().inbound_buffer_pool()
                                          : context_value->pool();
        const auto parsed_coding = request_content_coding(context_value->request_);
        if (const auto* invalid = parsed_coding.invalid()) {
            throw http_protocol_error(
                invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
        }
        if (parsed_coding.unsupported() != nullptr) {
            throw detail::unsupported_request_content_coding(http_supported_request_content_codings());
        }
        const auto codings = parsed_coding.codings();
        if (codings.empty()) {
            context_value->body_identity_ = true;
            co_return raw;
        }
        auto& decoded = context_value->decoded_body();
        decoded = decode_http_request_content(codings, raw,
            {.max_decoded_bytes_ = context_value->services().max_decoded_request_body_bytes(),
                .resource_ = decode_resource});
        context_value->body_decoded_ = true;
        co_return std::string_view(decoded);
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
}

task<std::span<const std::byte>> context_request::bytes() const {
    const auto run = [](const context* context_value) -> task<std::span<const std::byte>> {
        const auto body = co_await context_value->req().text();
        co_return std::as_bytes(std::span(body));
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
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
    const auto run = [](const context* context_value) -> task<std::pmr::vector<multipart_part>> {
        const auto boundary = get_multipart_boundary(context_value->request_);
        const auto request_body = co_await context_value->req().text();
        co_return parse_multipart_body(request_body, {.boundary_ = boundary, .resource_ = context_value->arena()});
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
}

task<void> context_request::discard_body() const {
    const auto run = [](const context* context_value) -> task<void> {
        if (const auto* lazy = context_value->request_body_source().lazy()) {
            co_await lazy->loader().discard();
            co_return;
        }
        if (const auto* streaming = context_value->request_body_source().streaming()) {
            while (co_await streaming->reader().read()) {
            }
        }
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
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
    const auto parsed_coding = request_content_coding(context_->request_);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(
            invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
    }
    const auto coded = std::ranges::any_of(
        parsed_coding.codings(), [](http_content_coding coding) { return coding != http_content_coding::identity; });
    if (parsed_coding.unsupported() != nullptr || coded) {
        throw detail::unsupported_request_content_coding("identity");
    }
    return ::ruvia::multipart_reader(body_reader(), {.boundary_ = boundary, .resource_ = context_->pool()});
}
}  // namespace ruvia
