#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_content_coding.h"
#include "http_field_validation.h"
#include "http_header.h"
#include "http_transfer_coding.h"

namespace ruvia::detail {

struct http_content_coding_field_result_access final {
    using sequence_type = http_content_coding_field_result::content_coding_sequence;

    [[nodiscard]] static constexpr http_content_coding_field_result coding(sequence_type values) noexcept {
        return http_content_coding_field_result(values);
    }

    [[nodiscard]] static constexpr http_content_coding_field_result unsupported(bool depth_exceeded = false) noexcept {
        return http_content_coding_field_result(http_unsupported_content_coding(depth_exceeded));
    }

    [[nodiscard]] static constexpr http_content_coding_field_result invalid() noexcept {
        return http_content_coding_field_result(http_invalid_content_coding_field{});
    }
};

// Shared token walk for decoding and outbound acceptance. The caller decides
// whether an unknown extension requires a decoder or only a quality check.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status visit_http_content_coding_field(std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_token_list(value, role, [&visitor](std::string_view token) {
        return visitor(token, http_content_coding_from_token(token));
    });
}

// Accumulates recipient list grammar across every Content-Encoding field line;
// empty list members are ignored.
class http_content_coding_field_parser final {
public:
    constexpr http_content_coding_field_parser() noexcept = default;

    void update(std::string_view value) noexcept;

    [[nodiscard]] http_content_coding_field_result finish() const noexcept;

private:
    http_content_coding_field_result_access::sequence_type codings_{};
    bool unknown_coding_{false};
    bool depth_exceeded_{false};
    bool invalid_{false};
};

// Validate list grammar without selecting decoders. Recipients/forwarders may
// retain empty members, but every nonempty member must still be a coding token.
[[nodiscard]] bool is_valid_http_content_encoding_field_value(std::string_view value, http_field_list_role role) noexcept;

template <typename headers_type>
[[nodiscard]] inline http_content_coding_field_result http_content_coding_from_headers(const headers_type& headers) noexcept {
    http_content_coding_field_parser parser;
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

enum class http_content_length_parse_status : std::uint8_t { ok,
    invalid,
    conflicting };

// Incremental Content-Length accumulator. Accepts comma-combined values with
// OWS and commits only when every repeated value agrees.
class http_content_length_state final {
public:
    [[nodiscard]] http_content_length_parse_status parse_field(std::string_view field_value) noexcept {
        auto status = http_content_length_parse_status::ok;
        bool saw_value = false;
        auto parsed_value = value_;
        http_visit_comma_separated_quoted_field_items(field_value, [&parsed_value, &status, &saw_value](std::string_view item) noexcept {
            status = accumulate(item, parsed_value);
            saw_value = true;
            return status == http_content_length_parse_status::ok;
        });
        if (status == http_content_length_parse_status::ok && !saw_value) {
            return http_content_length_parse_status::invalid;
        }
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed_value;
        }
        return status;
    }

    [[nodiscard]] http_content_length_parse_status parse_single_value(std::string_view input) noexcept {
        auto parsed = value_;
        const auto status = accumulate(input, parsed);
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed;
        }
        return status;
    }

    [[nodiscard]] std::optional<std::uint64_t> value() const noexcept {
        return value_;
    }

private:
    [[nodiscard]] static http_content_length_parse_status accumulate(std::string_view item, std::optional<std::uint64_t>& value) noexcept {
        if (item.empty()) {
            return http_content_length_parse_status::invalid;
        }
        std::uint64_t parsed_value = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed_value);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return http_content_length_parse_status::invalid;
        }
        if (value.has_value() && *value != parsed_value) {
            return http_content_length_parse_status::conflicting;
        }
        value = parsed_value;
        return http_content_length_parse_status::ok;
    }

    std::optional<std::uint64_t> value_;
};

[[nodiscard]] inline bool http_parse_transfer_coding_syntax(std::string_view item, std::string_view& coding, bool& has_parameters) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    coding = http_trim_ows(item.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }

    has_parameters = first_semicolon < item.size();
    return http_all_parameters(item, is_valid_http_transfer_parameter);
}

template <http_temporary_owning_char_string item_type>
bool http_parse_transfer_coding_syntax(item_type&&, std::string_view&, bool&) = delete;

enum class http_transfer_encoding_parse_status : std::uint8_t { ok,
    malformed,
    unsupported };

class http_non_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_non_chunked_transfer_encoding(http_transfer_codings transfer_codings) :
        transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_final_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_final_chunked_transfer_encoding(http_transfer_codings transfer_codings) :
        transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_transfer_encoding_value final {
public:
    [[nodiscard]] const http_non_chunked_transfer_encoding* non_chunked() const& noexcept {
        return std::get_if<http_non_chunked_transfer_encoding>(&value_);
    }
    const http_non_chunked_transfer_encoding* non_chunked() const&& = delete;

    [[nodiscard]] const http_final_chunked_transfer_encoding* final_chunked() const& noexcept {
        return std::get_if<http_final_chunked_transfer_encoding>(&value_);
    }
    const http_final_chunked_transfer_encoding* final_chunked() const&& = delete;

private:
    friend class http_transfer_encoding_state;

    using value_type = std::variant<http_non_chunked_transfer_encoding, http_final_chunked_transfer_encoding>;

    [[nodiscard]] static http_transfer_encoding_value make_non_chunked(http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_non_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    [[nodiscard]] static http_transfer_encoding_value make_final_chunked(http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_final_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    explicit http_transfer_encoding_value(http_non_chunked_transfer_encoding value) :
        value_(std::move(value)) {}

    explicit http_transfer_encoding_value(http_final_chunked_transfer_encoding value) :
        value_(std::move(value)) {}

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_transfer_encoding_value>);

// Field lines form one ordered recipient-side list. RFC 9110 section 5.6.1.2
// requires ignoring empty members, including on repeated field lines. Unknown
// codings are remembered while later items are still checked for malformed syntax.
class http_transfer_encoding_state final {
public:
    [[nodiscard]] http_transfer_encoding_parse_status parse_field(std::string_view field_value) {
        // The published value is the sole owner of the committed coding list.
        http_transfer_codings next_codings;
        bool final_chunked = false;
        if (value_) {
            if (const auto* chunked = value_->final_chunked()) {
                next_codings = chunked->transfer_codings();
                final_chunked = true;
            } else {
                next_codings = value_->non_chunked()->transfer_codings();
            }
        }
        bool unsupported = unsupported_;
        bool malformed = false;
        http_visit_comma_separated_quoted_field_items(field_value, [&next_codings, &final_chunked, &unsupported, &malformed](std::string_view item) {
            if (item.empty()) {
                return true;
            }
            std::string_view coding;
            bool has_parameters = false;
            if (final_chunked || !http_parse_transfer_coding_syntax(item, coding, has_parameters)) {
                malformed = true;
                return false;
            }
            if (http_ascii_equals_ignore_case(coding, "chunked")) {
                if (has_parameters) {
                    malformed = true;
                    return false;
                }
                final_chunked = true;
                return true;
            }
            const bool gzip = http_is_gzip_coding_token(coding);
            const bool deflate = http_ascii_equals_ignore_case(coding, "deflate");
            if (gzip || deflate) {
                if (has_parameters) {
                    malformed = true;
                    return false;
                }
                if (next_codings.size() == max_transfer_codings) {
                    unsupported = true;
                    return true;
                }
                next_codings.push_back(gzip ? http_transfer_coding::gzip : http_transfer_coding::deflate);
                return true;
            }
            unsupported = true;
            return true;
        });
        if (malformed) {
            return http_transfer_encoding_parse_status::malformed;
        }
        auto next_value = final_chunked ? http_transfer_encoding_value::make_final_chunked(std::move(next_codings)) : http_transfer_encoding_value::make_non_chunked(std::move(next_codings));
        // The coding list is inline, so publication cannot fail.
        value_.emplace(std::move(next_value));
        unsupported_ = unsupported;
        return unsupported_ ? http_transfer_encoding_parse_status::unsupported : http_transfer_encoding_parse_status::ok;
    }

    [[nodiscard]] const std::optional<http_transfer_encoding_value>& value() const noexcept {
        return value_;
    }
    [[nodiscard]] bool unsupported() const noexcept {
        return unsupported_;
    }

private:
    std::optional<http_transfer_encoding_value> value_;
    bool unsupported_{false};
};

[[nodiscard]] inline bool http_is_client_supported_te_transfer_coding(std::string_view coding) noexcept {
    return http_is_gzip_coding_token(coding) || http_ascii_equals_ignore_case(coding, "deflate");
}

[[nodiscard]] inline bool is_valid_http_te_field_item(std::string_view item, http_field_list_role role) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    const auto coding = http_trim_ows(item.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(coding, "trailers")) {
        return first_semicolon == item.size();
    }
    // RFC 9112 section 7.4: a client MUST NOT list chunked in TE. That binds
    // the sender; a recipient treats it as an always-acceptable coding.
    if (role == http_field_list_role::sender && !http_is_client_supported_te_transfer_coding(coding)) {
        return false;
    }

    // Validate transfer-parameter syntax and TE's weight in the same walk.
    // Unlike other parameters, q permits neither BWS around '=' nor a quoted
    // value. Recipients accept extension parameters; clients advertise only
    // the codings they can decode, with an optional weight.
    bool quality_seen = false;
    return http_all_parameters(item.substr(first_semicolon), [&quality_seen, role](std::string_view parameter) noexcept {
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const auto raw_name = parameter.substr(0, equals);
        const auto raw_value = parameter.substr(equals + 1);
        const auto name = http_trim_ows(raw_name);
        const auto value = http_trim_ows(raw_value);
        if (http_ascii_equals_ignore_case(name, "q")) {
            if (quality_seen || raw_name != name || raw_value != value || http_parse_quality_value(value) < 0) {
                return false;
            }
            quality_seen = true;
            return true;
        }
        return role == http_field_list_role::recipient && is_valid_http_transfer_parameter(parameter);
    });
}

[[nodiscard]] inline bool is_valid_http_te_field_value(std::string_view value, http_field_list_role role) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (http_trim_ows(value).empty()) {
        return true;
    }

    // A client generating TE is held to the sender's no-empty-member rule.
    return http_visit_list_members(value, role, [role](std::string_view item) noexcept {
        return is_valid_http_te_field_item(item, role) ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

// HTTP/2 carries no transfer codings, so TE may only advertise
// "trailers" (RFC 9113 section 8.2.2). The member is an
// unprefixed ABNF literal and therefore case-insensitive (RFC 7405 section
// 2.1); list members are OWS-trimmed (RFC 9110 section 5.6.1). Recipients
// ignore empty members and accept empty lists (RFC 9110 section 10.1.4).
// This sender API requires a nonempty list without empty members.
[[nodiscard]] inline bool is_valid_binary_http_te_field_value(std::string_view value, http_field_list_role role) noexcept {
    bool has_member = false;
    const auto status = http_visit_list_members(value, role, [&has_member](std::string_view member) noexcept {
        has_member = true;
        return http_ascii_equals_ignore_case(member, "trailers") ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    });
    return status == http_field_list_parse_status::ok && (role == http_field_list_role::recipient || has_member);
}

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

}  // namespace ruvia::detail
