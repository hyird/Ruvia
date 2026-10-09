#include "ruvia/http/http_response.h"

#include <array>
#include <charconv>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_response_head_policy.h"
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_coding.h"
#include "field/http_entity_tag.h"
#include "response/http_response_header_access.h"
#include "response/http_response_static_headers.h"
#include "response/response_header_utils.h"

namespace ruvia {

std::string_view http_response::body_bytes() const& noexcept {
    return detail::response_body(*this).bytes();
}

std::optional<http_response_file_view> http_response::file_body() const& noexcept {
    return detail::response_body(*this).file();
}

bool http_response::has_multipart_file_body() const noexcept {
    return detail::response_body(*this).multipart_body() != nullptr;
}

std::size_t http_response::body_segment_count() const noexcept {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        return multipart->segment_count();
    }
    return body.size() == 0 ? 0 : 1;
}

http_response_body_segment_view http_response::body_segment(std::size_t index) const& {
    const auto& body = detail::response_body(*this);
    if (const auto* multipart = body.multipart_body()) {
        if (index >= multipart->segment_count()) {
            throw std::out_of_range("response body segment index is out of range");
        }
        return multipart->segment(index);
    }
    if (index != 0) {
        throw std::out_of_range("response body segment index is out of range");
    }
    if (const auto file = body.file()) {
        return {.file_ = file};
    }
    return {.bytes_ = body.bytes()};
}

std::uint64_t http_response_body_plan::buffered_representation_length(const http_response& response) const noexcept {
    if (!status_allows_body() || content_semantics() == http_response_content_semantics::connect_tunnel) {
        return 0;
    }
    return static_cast<std::uint64_t>(detail::response_body(response).size());
}

bool http_buffered_response_write_plan::matches_response(const http_response& response) const noexcept {
    return response.status() == body_plan_.response_status() &&
           content_length_ == body_plan_.buffered_representation_length(response);
}

http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response.status());
    return http_buffered_response_write_plan(body_plan, body_plan.buffered_representation_length(response));
}

http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept {
    const auto policy = detail::get_response_write_policy(response_status);
    const auto semantics = detail::classify_http_response_content_semantics(request_method, response_status);
    const bool connect_tunnel = semantics == http_response_content_semantics::connect_tunnel;
    return http_response_body_plan(request_method, response_status, semantics, policy.body_allowed(),
        !policy.body_allowed() || semantics != http_response_content_semantics::with_content,
        !connect_tunnel && policy.auto_content_length_allowed(), !connect_tunnel && policy.explicit_content_length_allowed(),
        !connect_tunnel && policy.transfer_encoding_allowed());
}

namespace {

[[nodiscard]] std::pmr::string weak_etag_for_new_representation(
    std::string_view current_etag, std::pmr::memory_resource* resource) {
    std::pmr::string weak_etag(resource);
    if (detail::http_is_strong_etag(current_etag)) {
        weak_etag.reserve(current_etag.size() + 2);
        weak_etag.append("W/");
        weak_etag.append(current_etag.data(), current_etag.size());
    }
    return weak_etag;
}

}  // namespace

http_response::http_response()
    : http_response(options_type{}) {}

http_response::http_response(options_type options)
    : http_response(detail::http_resolved_pmr_resource_tag{},
          detail::http_pmr_resource_or_default(options.resource_)) {}

http_response::http_response(detail::http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : headers_(detail::http_resolved_pmr_resource_tag{}, resource) {}

http_response::http_response(http_response&& other) noexcept
    : status_code_(other.status_code_),
      known_header_bits_(other.known_header_bits_),
      known_header_indexes_(other.known_header_indexes_),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)) {
    other.known_header_bits_ = 0;
    other.known_header_indexes_.fill(0);
}

http_response& http_response::operator=(http_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    // A response is one resource domain. Member-wise assignment would retain the
    // target allocator in PMR alternatives while http_response_headers follows the
    // source resource, leaving one response split across unrelated request arenas.
    // Reconstructing transfers every owning alternative together and does not
    // allocate on the response hot path.
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::pmr::memory_resource* http_response::resource() const noexcept {
    return headers_.resource_;
}

std::pmr::memory_resource* http_response::memory_resource() const noexcept {
    return resource();
}

http_response http_response::clone_headers_for_transaction(std::size_t additional_headers) const {
    http_response clone(detail::http_resolved_pmr_resource_tag{}, resource());
    clone.status_code_ = status_code_;

    clone.headers_.reserve(headers_.size() + additional_headers);
    for (const auto& header : headers_) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either response may be changed or destroyed.
        auto copy = header.owned_
                        ? clone.headers_.make_owned_header(header.name(), header.value(), header.known_bit_)
                        : header;
        detail::set_response_header_append(copy, detail::response_header_append(header));
        (void)clone.headers_.append_prepared_header(copy);
    }
    clone.known_header_bits_ = known_header_bits_;
    clone.known_header_indexes_ = known_header_indexes_;

    return clone;
}

void http_response::commit_headers_from(http_response&& staged) noexcept {
    std::destroy_at(&headers_);
    ::new (static_cast<void*>(&headers_)) http_response_headers(std::move(staged.headers_));
    known_header_bits_ = staged.known_header_bits_;
    known_header_indexes_ = staged.known_header_indexes_;
}

http_response http_response::clone_for_transaction() const {
    auto clone = clone_headers_for_transaction();

    if (const auto* const borrowed_bytes = body_.borrowed_bytes()) {
        clone.body_.set_borrowed(borrowed_bytes->bytes());
    } else if (const auto* const static_bytes = body_.static_bytes()) {
        clone.body_.set_static(static_bytes->bytes());
    } else if (const auto* const owned_bytes = body_.owned_bytes()) {
        clone.body_.set_copy(clone.resource(), owned_bytes->bytes());
    } else if (const auto* const owned_file = body_.owned_file()) {
        clone.body_.set_owned_file(clone.resource(),
            detail::make_path_from_http_native_path(owned_file->native_path_c_str()), owned_file->size(),
            owned_file->offset(), owned_file->length(), owned_file->identity());
    } else if (const auto* const borrowed_file = body_.borrowed_file()) {
        clone.body_.set_borrowed_file(borrowed_file->native_path_c_str(), borrowed_file->size(),
            borrowed_file->offset(), borrowed_file->length(), borrowed_file->identity());
    } else if (const auto* const multipart = body_.multipart_body()) {
        clone.body_.set_multipart(clone.resource(),
            multipart->file().to_path(), multipart->file_size(),
            multipart->identity(), multipart->plan().clone(clone.resource()));
    }

    return clone;
}

http_status_code http_response::status() const noexcept {
    return status_code_;
}

const http_response_headers& http_response::headers() const& noexcept {
    return headers_;
}

void http_response::status(http_status_code status_code) {
    if (status_code == http_status::switching_protocols) {
        throw std::invalid_argument("Switching Protocols requires a dedicated protocol driver");
    }
    if (!detail::http_final_status_code_valid(status_code)) {
        throw std::invalid_argument("invalid final HTTP status code");
    }
    status_code_ = status_code;
}

void http_response::transfer_headers_from(
    const http_response& source_value, http_response_header_transfer mode) {
    auto staged = clone_headers_for_transaction(source_value.headers().size());
    bool removed_cookies = false;
    for (const auto& header : source_value.headers()) {
        const auto known_bit = detail::response_header_known_bit(header);
        if (mode == http_response_header_transfer::assign &&
            known_bit == detail::response_header_content_type) {
            continue;
        }
        const auto name = header.name();
        const auto value = header.value();
        if (known_bit == detail::response_header_set_cookie) {
            if (mode == http_response_header_transfer::assign && !removed_cookies) {
                staged.remove_header("Set-Cookie");
                removed_cookies = true;
            }
            staged.header(name, value, {.mode_ = http_response_header_mode::append});
        } else if (detail::response_header_append(header)) {
            const auto occurrence_through = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source_value.headers()) {
                    if (http_ascii_equals_ignore_case(candidate.name(), name) && candidate.value() == value) {
                        ++count;
                    }
                    if (&candidate == &header) {
                        break;
                    }
                }
                return count;
            }();
            const auto existing_count = [&] {
                std::size_t count = 0;
                for (const auto& candidate : staged.headers()) {
                    if (http_ascii_equals_ignore_case(candidate.name(), name) && candidate.value() == value) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existing_count < occurrence_through) {
                staged.header(name, value, {.mode_ = http_response_header_mode::append});
            }
        } else if (mode == http_response_header_transfer::merge) {
            if (!staged.header(name)) {
                staged.header(name, value);
            }
        } else {
            staged.header(name, value);
        }
    }
    commit_headers_from(std::move(staged));
}

void http_response::body(std::string_view value) {
    body_.set_copy(resource(), value);
}

void http_response::owned_body(std::pmr::string&& value) {
    set_body_owned(std::move(value));
}

void http_response::static_body(std::string_view value) noexcept {
    set_body_static_view(value);
}

void http_response::set_body_borrowed_view(std::string_view value) noexcept {
    body_.set_borrowed(value);
}

void http_response::set_body_static_view(std::string_view value) noexcept {
    body_.set_static(value);
}

void http_response::set_body_owned(std::pmr::string&& value) {
    body_.set_owned(resource(), std::move(value));
}

// Owns only newly staged descriptors, never copies the response's existing
// header block. Publication transfers descriptors; unwinding releases the rest.
class http_response::encoded_header_update final {
public:
    explicit encoded_header_update(http_response& owner_value) noexcept
        : owner_(owner_value) {}
    encoded_header_update(const encoded_header_update&) = delete;
    encoded_header_update& operator=(const encoded_header_update&) = delete;

    ~encoded_header_update() noexcept {
        for (std::size_t slot = 0; slot < prepared_.size(); ++slot) {
            if (active_[slot]) {
                owner_.headers_.release_header(prepared_[slot]);
            }
        }
    }

    void stage(std::size_t slot, std::string_view name, std::string_view value,
        std::uint32_t known_bit) {
        const auto builtin = http_response_headers::make_static_header(name, value, known_bit);
        prepared_[slot] = builtin ? *builtin : owner_.headers_.make_owned_header(name, value, known_bit);
        active_[slot] = true;
    }

    void commit(std::size_t slot, std::string_view name, std::uint32_t known_bit) noexcept {
        if (auto* const existing = owner_.find_header_for_update(name, known_bit)) {
            const bool was_appended = detail::response_header_append(*existing);
            owner_.headers_.release_header(*existing);
            *existing = prepared_[slot];
            active_[slot] = false;
            if (was_appended) {
                (void)owner_.collapse_response_headers(*existing, known_bit);
            }
            return;
        }
        const auto index = owner_.headers_.size();
        (void)owner_.headers_.append_prepared_header(prepared_[slot]);
        active_[slot] = false;
        owner_.record_known_header_index(known_bit, index);
    }

private:
    http_response& owner_;
    std::array<http_response_header, 3> prepared_{};
    std::array<bool, 3> active_{};
};

void http_response::apply_content_encoding(std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, nullptr);
}

void http_response::replace_body_with_content_encoding(
    std::pmr::string&& value, std::string_view content_encoding) {
    apply_encoded_representation(content_encoding, &value);
}

void http_response::apply_encoded_representation(
    std::string_view content_encoding, std::pmr::string* body) {
    detail::validate_response_header_storage_size(std::string_view("Content-Encoding").size(), content_encoding.size());
    if (!detail::is_valid_http_content_encoding_field_value(content_encoding, detail::http_field_list_role::sender)) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    constexpr std::size_t encoding_header = 0;
    constexpr std::size_t etag_header = 1;
    constexpr std::size_t length_header = 2;
    const auto weak_etag = weak_etag_for_new_representation(
        known_header_value(detail::response_header_etag), resource());
    const std::array<std::pair<std::string_view, std::uint32_t>, 3> fields_value{{
        {"Content-Encoding", detail::response_header_content_encoding},
        {"ETag", detail::response_header_etag},
        {"Content-Length", detail::response_header_content_length},
    }};
    const auto selected = [&](std::size_t slot) noexcept {
        return (slot != etag_header || !weak_etag.empty()) &&
               (slot != length_header || body != nullptr);
    };
    std::size_t missing_headers = 0;
    for (std::size_t slot = 0; slot < fields_value.size(); ++slot) {
        if (selected(slot) && find_header_for_read(fields_value[slot].first, fields_value[slot].second) == nullptr) {
            ++missing_headers;
        }
    }
    headers_.reserve(headers_.size() + missing_headers);

    encoded_header_update update(*this);
    update.stage(encoding_header, fields_value[encoding_header].first, content_encoding,
        fields_value[encoding_header].second);
    if (selected(etag_header)) {
        update.stage(etag_header, fields_value[etag_header].first, weak_etag, fields_value[etag_header].second);
    }
    if (body != nullptr) {
        std::array<char, 32> length_buffer{};
        const auto [length_end, length_error] = std::to_chars(
            length_buffer.data(), length_buffer.data() + length_buffer.size(), body->size());
        if (length_error != std::errc{}) {
            throw std::logic_error("failed to format encoded response length");
        }
        update.stage(length_header, fields_value[length_header].first,
            std::string_view(length_buffer.data(), static_cast<std::size_t>(length_end - length_buffer.data())),
            fields_value[length_header].second);
        // All descriptors exist before replacing the body. set_owned has a strong
        // failure guarantee; everything after it is no-throw publication.
        body_.set_owned(resource(), std::move(*body));
    }
    update.commit(encoding_header, fields_value[encoding_header].first, fields_value[encoding_header].second);
    if (body == nullptr) {
        (void)remove_header_validated("Content-Length", detail::response_header_content_length);
    }
    if (selected(etag_header)) {
        update.commit(etag_header, fields_value[etag_header].first, fields_value[etag_header].second);
    }
    if (body != nullptr) {
        update.commit(length_header, fields_value[length_header].first, fields_value[length_header].second);
    }
}

void http_response::materialize_body() {
    body_.materialize(resource());
}

void http_response::file_body(std::filesystem::path file, std::uint64_t size,
    std::uint64_t offset, std::uint64_t length, http_response_file_identity identity) {
    set_file_body(std::move(file), size, offset, length, identity);
}

void http_response::multipart_file_body(std::filesystem::path file, std::uint64_t size,
    http_response_file_identity identity, http_multipart_byte_range_plan&& plan) {
    body_.set_multipart(resource(), file, size, identity, std::move(plan));
}

void http_response::content_range(
    std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    set_content_range(offset, length, size);
}

void http_response::content_range_unsatisfied(std::uint64_t size) {
    set_content_range_unsatisfied(size);
}

void http_response::add_vary_token(std::string_view token) {
    detail::add_vary_token(*this, token);
}

void http_response::set_file_body(std::filesystem::path file, std::uint64_t size) {
    set_file_body(std::move(file), size, 0, size);
}

void http_response::set_file_body(
    std::filesystem::path file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    set_file_body(std::move(file), size, offset, length, http_response_file_identity::unchecked());
}

void http_response::set_file_body(std::filesystem::path file, std::uint64_t size, std::uint64_t offset,
    std::uint64_t length, http_response_file_identity identity) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_owned_file(resource(), file, size, offset, length, identity);
}

void http_response::set_borrowed_file_body(const std::filesystem::path& file, std::uint64_t size) {
    set_borrowed_file_body(file, size, 0, size);
}

void http_response::set_borrowed_file_body(const std::filesystem::path& file, std::uint64_t size,
    std::uint64_t offset, std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_borrowed_file(file.c_str(), size, offset, length);
}

void http_response::set_borrowed_native_file_body(
    const detail::http_native_path_char_type* file, std::uint64_t size) {
    set_borrowed_native_file_body(file, size, 0, size);
}

void http_response::set_borrowed_native_file_body(const detail::http_native_path_char_type* file,
    std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file == nullptr || *file == detail::http_native_path_char_type{}) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.set_borrowed_file(file, size, offset, length);
}

}  // namespace ruvia
