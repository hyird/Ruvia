#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_response.h"

#include "field/response_field_validation.h"
#include "response/http_response_header_storage.h"
#include "response/http_response_static_headers.h"

namespace ruvia {
namespace {

// Every insertion path validates one field line completely, so a serialized
// head only re-checks the rules that span several lines. Returns the field's
// known bit.
[[nodiscard]] std::uint32_t validate_response_header_field(std::string_view key, std::string_view value) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!is_valid_http_header_value(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto known_bit = detail::classify_response_header_name(key);
    const auto kind = detail::classify_response_field(key, known_bit);
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    switch (detail::parse_http_response_control_field(kind, value, connection_options, upgrade_protocols)) {
        case detail::http_response_control_field_status::invalid_connection:
            throw std::invalid_argument("invalid HTTP Connection header");
        case detail::http_response_control_field_status::invalid_upgrade:
            throw std::invalid_argument("invalid HTTP Upgrade header");
        case detail::http_response_control_field_status::te_forbidden:
            throw std::invalid_argument("TE is not a response field");
        case detail::http_response_control_field_status::ok:
            break;
    }
    detail::response_field_state fields;
    if (detail::validate_response_field(kind, value, fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP response field value");
    }
    return known_bit;
}

[[nodiscard]] bool response_header_mode_appends(http_response_header_mode mode) {
    switch (mode) {
        case http_response_header_mode::replace:
            return false;
        case http_response_header_mode::append:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

http_response_headers http_response_headers::clone(std::size_t additional_headers) const {
    const auto existing = size();
    if (additional_headers > heap_.max_size() - existing) {
        throw std::length_error("HTTP response header table is too large");
    }
    http_response_headers copy(detail::http_resolved_pmr_resource_tag{}, resource_);
    copy.reserve(existing + additional_headers);
    for (const auto& header_value : *this) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either block may be changed or destroyed.
        auto descriptor = header_value.owned_
                              ? copy.make_owned_header(header_value.name(), header_value.value(), header_value.known_bit_)
                              : header_value;
        descriptor.append_ = header_value.append_;
        (void)copy.append_prepared_header(descriptor);
    }
    copy.known_header_bits_ = known_header_bits_;
    if (known_header_bits_ != 0) {
        copy.known_header_indexes_ = known_header_indexes_;
    }
    return copy;
}

void http_response_headers::commit(http_response_headers&& staged) noexcept {
    if (this == &staged) {
        return;
    }
    // Reconstruction adopts the staged table without allocating; the move
    // leaves `staged` empty with its own resource and a cleared index.
    std::destroy_at(this);
    std::construct_at(this, std::move(staged));
}

bool http_response_headers::try_append_disjoint(
    const http_response_headers& source, http_response_header_transfer mode) {
    if (&source == this) {
        return false;
    }
    const auto skips = [mode](std::uint32_t known_bit) noexcept {
        return mode == http_response_header_transfer::assign && known_bit == detail::response_header_content_type;
    };
    // A source block already holds each field under its own rules: one line per
    // single-valued field and key-unique cookies. When no source field names a
    // field this block holds, every transfer mode keeps each source line as is.
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (skips(known_bit)) {
            continue;
        }
        if (known_bit != 0 ? (known_header_bits_ & known_bit) != 0
                           : find_header_for_read(header_value.name(), 0) != nullptr) {
            return false;
        }
    }

    reserve(size() + source.size());
    const auto first_index = size();
    try {
        for (const auto& header_value : source) {
            const auto known_bit = header_value.known_bit_;
            if (skips(known_bit)) {
                continue;
            }
            // Static descriptors are shared; owning descriptors get an
            // independent allocation, as in clone().
            auto descriptor = header_value.owned_
                                  ? make_owned_header(header_value.name(), header_value.value(), known_bit)
                                  : header_value;
            // Transferred cookies are appended occurrences, like the staged path.
            descriptor.append_ = header_value.append_ || known_bit == detail::response_header_set_cookie;
            (void)append_prepared_header(descriptor);
        }
    } catch (...) {
        auto* const first = data() + first_index;
        for (auto* cursor_value = first; cursor_value != end(); ++cursor_value) {
            release_header(*cursor_value);
        }
        truncate(first);
        throw;
    }
    const auto* const items = data();
    for (auto index = first_index; index < size(); ++index) {
        if (const auto known_bit = items[index].known_bit_; known_bit != 0) {
            record_known_header_index(known_bit, index);
        }
    }
    return true;
}

void http_response_headers::transfer_from(
    const http_response_headers& source, http_response_header_transfer mode) {
    if (try_append_disjoint(source, mode)) {
        return;
    }
    // Fields that meet existing ones are resolved on a staged copy, so a
    // failure leaves this block unchanged.
    auto staged = clone(source.size());
    bool removed_cookies = false;
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (mode == http_response_header_transfer::assign &&
            known_bit == detail::response_header_content_type) {
            continue;
        }
        const auto name = header_value.name();
        const auto value = header_value.value();
        if (known_bit == detail::response_header_set_cookie) {
            if (mode == http_response_header_transfer::assign && !removed_cookies) {
                (void)staged.remove_header_validated("Set-Cookie", detail::response_header_set_cookie);
                removed_cookies = true;
            }
            staged.upsert_set_cookie_header_validated(value);
        } else if (detail::http_response_header_access::append(header_value)) {
            const auto matches_header = [&](const http_response_header& candidate) noexcept {
                return detail::response_header_matches(candidate, name, known_bit) && candidate.value() == value;
            };
            const auto occurrence_through = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source) {
                    if (matches_header(candidate)) {
                        ++count;
                    }
                    if (&candidate == &header_value) {
                        break;
                    }
                }
                return count;
            }();
            const auto existing_count = [&] {
                std::size_t count = 0;
                for (const auto& candidate : std::as_const(staged)) {
                    if (matches_header(candidate)) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existing_count < occurrence_through) {
                staged.append_header_validated(name, value, known_bit);
            }
        } else if (mode == http_response_header_transfer::merge) {
            if (staged.find_header_for_read(name, known_bit) == nullptr) {
                if (!header_value.owned_) {
                    staged.publish_header(header_value);
                } else {
                    staged.set_header_validated(name, value, known_bit);
                }
            }
        } else if (!header_value.owned_) {
            staged.publish_header(header_value);
        } else {
            staged.set_header_validated(name, value, known_bit);
        }
    }
    commit(std::move(staged));
}

void http_response_headers::record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept {
    if (known_bit == 0 || (known_header_bits_ & known_bit) != 0) {
        return;
    }
    known_header_bits_ |= known_bit;
    const auto slot = detail::response_known_header_slot(known_bit);
    if (slot >= known_header_indexes_.size()) {
        return;
    }
    // A slot holds the first occurrence's index + 1; 0 (unset) and -1 (index
    // beyond int16) both fall back to a scan.
    known_header_indexes_[slot] = index < static_cast<std::size_t>((std::numeric_limits<std::int16_t>::max)())
                                      ? static_cast<std::int16_t>(index + 1)
                                      : std::int16_t{-1};
}

http_response_header* http_response_headers::find_header_for_update(
    std::string_view key, std::uint32_t known_bit) noexcept {
    return const_cast<http_response_header*>(std::as_const(*this).find_header_for_read(key, known_bit));
}

const http_response_header* http_response_headers::find_header_for_read(
    std::string_view key, std::uint32_t known_bit) const noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return nullptr;
    }
    if (const auto slot = detail::response_known_header_slot(known_bit);
        slot < known_header_indexes_.size() && known_header_indexes_[slot] > 0) {
        return begin() + (known_header_indexes_[slot] - 1);
    }
    for (const auto& header_value : *this) {
        if (detail::response_header_matches(header_value, key, known_bit)) {
            return &header_value;
        }
    }
    return nullptr;
}

http_response_header& http_response_headers::prepare_header_value_storage(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    if (auto* const header_value = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*header_value);
        assign_uninitialized_value(*header_value, key, value_size, known_bit);
        return was_appended ? collapse_response_headers(*header_value, known_bit) : *header_value;
    }

    const auto index = size();
    auto& header_value = add_uninitialized_value(key, value_size, known_bit);
    record_known_header_index(known_bit, index);
    return header_value;
}

std::optional<std::string_view> http_response_headers::header(std::string_view name) const& noexcept {
    if (const auto* const found =
            find_header_for_read(name, detail::classify_response_header_name(name))) {
        return found->value();
    }
    return std::nullopt;
}

void http_response_headers::rebuild_known_header_index() noexcept {
    known_header_bits_ = 0;
    const auto* const first = data();
    const auto* const last = first + size();
    for (auto* cursor_value = first; cursor_value != last; ++cursor_value) {
        const auto known_bit = detail::http_response_header_access::known_bit(*cursor_value);
        if (known_bit != 0) {
            record_known_header_index(known_bit, static_cast<std::size_t>(cursor_value - first));
        }
    }
}

void http_response_headers::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void http_response_headers::content_type(http_content_type type) {
    publish_header(detail::static_content_type_header(type));
}

http_response_header& http_response_headers::replace_header(
    http_response_header& existing, http_response_header prepared) noexcept {
    const bool was_appended = detail::http_response_header_access::append(existing);
    release_header(existing);
    existing = prepared;
    return was_appended ? collapse_response_headers(existing, prepared.known_bit_) : existing;
}

void http_response_headers::publish_header(http_response_header prepared) {
    const auto known_bit = prepared.known_bit_;
    if (auto* const existing = find_header_for_update(prepared.name(), known_bit)) {
        (void)replace_header(*existing, prepared);
        return;
    }
    const auto index = size();
    (void)append_header(prepared);
    record_known_header_index(known_bit, index);
}

void http_response_headers::header(
    std::string_view key, std::string_view value, http_response_header_options options) {
    const auto known_bit = validate_response_header_field(key, value);
    if (response_header_mode_appends(options.mode_)) {
        if (detail::response_header_append_forbidden(known_bit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (known_bit == detail::response_header_set_cookie) {
            upsert_set_cookie_header_validated(value);
        } else {
            append_header_validated(key, value, known_bit);
        }
    } else {
        set_header_validated(key, value, known_bit);
    }
}

void http_response_headers::remove_header(std::string_view key) {
    detail::validate_response_header_storage_size(key.size(), 0);
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)remove_header_validated(key, detail::classify_response_header_name(key));
}

void http_response_headers::set_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    if (auto* const header_value = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*header_value);
        assign(*header_value, key, value, known_bit);
        if (was_appended) {
            (void)collapse_response_headers(*header_value, known_bit);
        }
        return;
    }

    const auto index = size();
    add(key, value, known_bit);
    record_known_header_index(known_bit, index);
}

void http_response_headers::append_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    auto& header_value = append_header_uninitialized_value(key, value.size(), known_bit);
    // Appending can move descriptors, but existing header byte blocks stay
    // stable, including any bytes borrowed by value.
    if (!value.empty()) {
        std::memcpy(detail::http_response_header_access::value_begin(header_value), value.data(), value.size());
    }
}

http_response_header& http_response_headers::append_header_uninitialized_value(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value_size);
    if (detail::response_header_append_forbidden(known_bit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: the new header
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing block exactly as it was.
    const auto* const existing = find_header_for_read(key, known_bit);
    const auto existing_index = existing == nullptr
                                    ? std::optional<std::size_t>{}
                                    : std::optional{static_cast<std::size_t>(existing - data())};
    const auto index = size();
    auto& header_value = add_uninitialized_value(key, value_size, known_bit);
    if (existing_index) {
        // Appending may move the descriptor table. Preserve an index, not a pointer,
        // to mark the existing entry without repeating the header lookup.
        detail::http_response_header_access::set_append(data()[*existing_index]);
    }
    // Mark the append flag so a later merge of this block keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::http_response_header_access::set_append(header_value);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::collapse_response_headers(
    http_response_header& retained, std::uint32_t known_bit) noexcept {
    const auto key = retained.name();
    auto* const first = data();
    auto* const last = first + size();
    auto* const retained_address = &retained;
    auto* collapsed_retained = retained_address;
    auto* write = first;
    for (auto* read = first; read != last; ++read) {
        if (read != retained_address && detail::response_header_matches(*read, key, known_bit)) {
            release_header(*read);
            continue;
        }
        if (read == retained_address) {
            collapsed_retained = write;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != last) {
        truncate(write);
    }
    rebuild_known_header_index();
    return *collapsed_retained;
}

bool http_response_headers::remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return false;
    }
    auto* const first = data();
    auto* const last = first + size();
    auto* write = first;
    bool removed = false;
    http_response_header retired_key_header;

    for (auto* read = first; read != last; ++read) {
        if (detail::response_header_matches(*read, key, known_bit)) {
            if (!removed) {
                retired_key_header = std::exchange(*read, http_response_header{});
                key = retired_key_header.name();
            } else {
                release_header(*read);
            }
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    release_header(retired_key_header);
    truncate(write);
    rebuild_known_header_index();
    return true;
}

void http_response_headers::header_stable_view(std::string_view key, std::string_view value) {
    const auto known_bit = validate_response_header_field(key, value);
    if (auto* const header_value = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*header_value);
        assign_stable_view(*header_value, key, value, known_bit);
        if (was_appended) {
            (void)collapse_response_headers(*header_value, known_bit);
        }
        return;
    }

    const auto index = size();
    add_stable_view(key, value, known_bit);
    record_known_header_index(known_bit, index);
}

void http_response_headers::add_vary_token(std::string_view token) {
    // Vary is a list field (RFC 9110 section 12.5.5): wildcard and membership
    // are decided across every line, not only the indexed first occurrence.
    token = http_trim_ows(token);
    if (token.empty()) {
        return;
    }
    const bool wildcard_token = token == "*";

    // The existing non-empty lines comma-joined in wire order, collected while
    // checking membership. Short lists stay on the stack; the PMR string takes
    // over once they outgrow it.
    std::array<char, 256> inline_joined;
    std::size_t inline_size = 0;
    std::pmr::string spilled_joined(resource_);
    const auto append_joined = [&](std::string_view piece) {
        if (spilled_joined.empty() && piece.size() <= inline_joined.size() - inline_size) {
            std::memcpy(inline_joined.data() + inline_size, piece.data(), piece.size());
            inline_size += piece.size();
            return;
        }
        if (spilled_joined.empty()) {
            spilled_joined.assign(inline_joined.data(), inline_size);
        }
        spilled_joined.append(piece);
    };
    if ((known_header_bits_ & detail::response_header_vary) != 0) {
        for (const auto& header_value : *this) {
            if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
                continue;
            }
            const auto value = header_value.value();
            if (detail::http_has_token(value, "*") || (!wildcard_token && detail::http_has_token(value, token))) {
                return;
            }
            if (!http_trim_ows(value).empty()) {
                append_joined(value);
                append_joined(", ");
            }
        }
    }

    if (wildcard_token) {
        header("Vary", "*");
        return;
    }
    if (!is_valid_http_header_name(token)) {
        throw std::invalid_argument("invalid HTTP Vary header token");
    }
    append_joined(token);
    header("Vary", spilled_joined.empty() ? std::string_view(inline_joined.data(), inline_size)
                                          : std::string_view(spilled_joined));
}

}  // namespace ruvia
