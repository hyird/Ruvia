#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_request.h"

namespace ruvia {

class set_cookie_plan;

enum class http_response_header_mode : std::uint8_t {
    replace,
    append,
};

enum class http_response_header_transfer : std::uint8_t {
    merge,
    assign,
    apply,
};

// Content-Type values whose field lines are constants of this library. Setting
// one shares a static descriptor: no allocation and no grammar re-validation.
enum class http_content_type : std::uint8_t {
    text_plain_utf8,   // text/plain; charset=UTF-8
    text_html_utf8,    // text/html; charset=UTF-8
    application_json,  // application/json
};

struct http_response_header_options {
    http_response_header_mode mode_{http_response_header_mode::replace};
};

namespace detail {

struct http_response_body_access;
struct http_response_header_access;
struct http_response_header_state_access;

}  // namespace detail

struct http_response_header {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return bytes_ == nullptr ? std::string_view{} : std::string_view(bytes_, name_size_);
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return bytes_ == nullptr ? std::string_view{} : std::string_view(bytes_ + name_size_, value_size_);
    }

private:
    friend class http_response;
    friend class http_response_headers;
    friend struct detail::http_response_header_access;

    http_response_header() noexcept = default;

    const char* bytes_{nullptr};
    std::uint32_t name_size_{0};
    std::uint32_t value_size_{0};
    std::uint32_t known_bit_{0};
    bool owned_{false};
    bool append_{false};
};

static_assert(std::is_trivially_copyable_v<http_response_header>);
static_assert(sizeof(http_response_header) <= 24);

// Owns a response header block: descriptor storage, the known-header index, every
// field mutation rule, and descriptor staging. A clone is a header-only
// transaction: edit it around external operations, then publish it with
// http_response::commit_headers.
class http_response_headers final {
public:
    using value_type = http_response_header;
    using const_iterator = const http_response_header*;

    // Moving transfers every descriptor and the index. The source remains
    // usable, independent, and empty.
    http_response_headers(http_response_headers&& other) noexcept;
    http_response_headers(const http_response_headers&) = delete;
    http_response_headers& operator=(const http_response_headers&) = delete;
    http_response_headers& operator=(http_response_headers&&) = delete;
    ~http_response_headers();

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data() + size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // Inputs may borrow current header storage. Refresh views after mutation.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    // Validated like header(). Name and value may be retained as borrowed views
    // without copying. Keep their bytes valid and unchanged while this block or
    // any clone retains them.
    void header_stable_view(std::string_view key, std::string_view value);
    // Replaces Content-Type with a library constant.
    void content_type(http_content_type type);
    void remove_header(std::string_view key);
    // Replace cookies with the same name, Domain/Path scope, and Partitioned
    // state; partitioned and unpartitioned cookies may coexist.
    void set_cookie(const set_cookie_plan& plan);
    void allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void content_range(http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve(std::size_t count);
    void transfer_from(const http_response_headers& source, http_response_header_transfer mode);
    // Owned bytes are copied so either block may change or be destroyed;
    // stable views are shared. Reserves room for additional_headers more fields.
    // Throws length_error before cloning if the total field count cannot fit.
    [[nodiscard]] http_response_headers clone(std::size_t additional_headers = 0) const;

private:
    friend class http_response;
    friend struct detail::http_response_header_state_access;

    using iterator = http_response_header*;

    static constexpr std::size_t known_header_count = 22;
    static constexpr std::size_t inline_capacity = 8;
    struct inline_storage_type {
        alignas(http_response_header) std::byte bytes_[sizeof(http_response_header)];
    };

    // Stages up to three owned descriptors without touching the table and
    // publishes them later without allocation or failure.
    class descriptor_stage;

    http_response_headers() noexcept;

    // Mutable iteration for the owning block and its friends. Named apart from
    // the public const begin()/end() so a non-const block still iterates
    // through the public overloads.
    [[nodiscard]] iterator mutable_begin() & noexcept {
        return data();
    }

    [[nodiscard]] iterator mutable_end() & noexcept {
        return data() + size();
    }

    // Header-only transaction publication. `staged` is left empty and independent.
    void commit(http_response_headers&& staged) noexcept;
    // Appends the source's fields in place when none can interact with an
    // existing field. Returns false, leaving this block unchanged, otherwise.
    [[nodiscard]] bool try_append_disjoint(const http_response_headers& source, http_response_header_transfer mode);
    // Replaces the field's line with `prepared`, or appends it; ownership of
    // `prepared` transfers either way.
    void publish_header(http_response_header prepared);
    // Releases `existing`, adopts `prepared` in its slot, and collapses the
    // field's other lines when `existing` was one of several.
    http_response_header& replace_header(http_response_header& existing, http_response_header prepared) noexcept;

    // Field rules and the known-header index.
    void record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept;
    void rebuild_known_header_index() noexcept;
    [[nodiscard]] http_response_header* find_header_for_update(std::string_view key, std::uint32_t known_bit) noexcept;
    [[nodiscard]] const http_response_header* find_header_for_read(std::string_view key, std::uint32_t known_bit) const noexcept;
    // Owns lookup, duplicate collapse, and index publication for every upsert.
    // Mutation receives the retained slot, or nullptr when it must append.
    template <typename Mutation>
    http_response_header& upsert_header(std::string_view key, std::uint32_t known_bit, Mutation&& mutate);
    http_response_header& prepare_header_value_storage(std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    void set_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void append_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit);
    http_response_header& append_header_uninitialized_value(std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] http_response_header& collapse_response_headers(http_response_header& retained, std::uint32_t known_bit) noexcept;
    bool remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept;
    void upsert_set_cookie_header_validated(std::string_view value);
    [[nodiscard]] http_response_header* find_set_cookie_header(std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path, std::string_view domain, bool partitioned) noexcept;
    void erase_later_set_cookie_headers(http_response_header& retained, std::string_view cookie_name, bool has_path, std::string_view path, std::string_view domain, bool partitioned) noexcept;
    // Descriptor storage.
    [[nodiscard]] http_response_header* inline_data() noexcept;
    [[nodiscard]] const http_response_header* inline_data() const noexcept;
    [[nodiscard]] http_response_header* data() noexcept;
    [[nodiscard]] const http_response_header* data() const noexcept;
    [[nodiscard]] http_response_header make_owned_header(std::string_view name, std::string_view value, std::uint32_t known_bit);
    [[nodiscard]] http_response_header make_uninitialized_header(std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] static std::optional<http_response_header> make_static_header(std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] std::optional<http_response_header> make_shared_header(std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept;
    [[nodiscard]] bool try_assign_owned_in_place(http_response_header& header, std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] bool borrows_owned_header(std::string_view name, std::string_view value) const noexcept;
    http_response_header& append_header(http_response_header header);
    http_response_header& add_uninitialized_value(std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    http_response_header& add(std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void assign(http_response_header& header, std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    http_response_header& assign_uninitialized_value(http_response_header& header, std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    void release_header(http_response_header& header) noexcept;
    // Drops every header from new_end onward; the dropped headers must
    // already be released.
    void truncate(iterator new_end) noexcept;
    http_response_header& append_prepared_header(http_response_header header) noexcept;
    void clear() noexcept;
    void spill(std::size_t min_capacity);
    void move_from(http_response_headers&& other) noexcept;

    std::vector<http_response_header> heap_;
    std::array<inline_storage_type, inline_capacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
    std::uint32_t known_header_bits_{0};
    std::array<std::int16_t, known_header_count> known_header_indexes_;
};

}  // namespace ruvia
