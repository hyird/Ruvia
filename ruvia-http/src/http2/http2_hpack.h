#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/hpack_protocol_types.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

struct hpack_decoder_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

struct hpack_static_index final {
    static constexpr std::uint32_t status_ok = 8;
    static constexpr std::uint32_t status_no_content = 9;
    static constexpr std::uint32_t status_partial_content = 10;
    static constexpr std::uint32_t status_not_modified = 11;
    static constexpr std::uint32_t status_bad_request = 12;
    static constexpr std::uint32_t status_not_found = 13;
    static constexpr std::uint32_t status_internal_server_error = 14;
    static constexpr std::uint32_t accept_ranges = 18;
    static constexpr std::uint32_t access_control_allow_origin = 20;
    static constexpr std::uint32_t allow = 22;
    static constexpr std::uint32_t cache_control = 24;
    static constexpr std::uint32_t content_encoding = 26;
    static constexpr std::uint32_t content_length = 28;
    static constexpr std::uint32_t content_range = 30;
    static constexpr std::uint32_t content_type = 31;
    static constexpr std::uint32_t date = 33;
    static constexpr std::uint32_t etag = 34;
    static constexpr std::uint32_t last_modified = 44;
    static constexpr std::uint32_t location = 46;
    static constexpr std::uint32_t server = 54;
    static constexpr std::uint32_t set_cookie = 55;
    static constexpr std::uint32_t vary = 59;
};

// HPACK literal representation prefixes (RFC 7541 §6.2).
inline constexpr std::uint8_t hpack_literal_without_indexing = 0x00;  // §6.2.2
inline constexpr std::uint8_t hpack_literal_never_indexed = 0x10;     // §6.2.3

// Single source of truth for which header fields must never be committed to an
// HPACK dynamic table. RFC 7541 §7.1.3: credential-bearing fields SHOULD use the
// never-indexed literal so that an intermediary along the path never places them
// in a shared dynamic table (a compression side-channel, cf. CRIME). Names must
// already be lowercased (both encode call sites lowercase before dispatch).
[[nodiscard]] inline bool hpack_header_name_is_sensitive(std::string_view name) noexcept {
    return name == "authorization" || name == "proxy-authorization" || name == "cookie" ||
           name == "set-cookie";
}

class hpack_decoder final {
public:
    using header_callback_type = bool (*)(void*, std::string_view, std::string_view);

    class decode_transaction_type final {
    public:
        decode_transaction_type(const decode_transaction_type&) = delete;
        decode_transaction_type& operator=(const decode_transaction_type&) = delete;
        decode_transaction_type(decode_transaction_type&&) = delete;
        decode_transaction_type& operator=(decode_transaction_type&&) = delete;
        ~decode_transaction_type();

        void commit() noexcept;
        void rollback() noexcept;
        [[nodiscard]] bool active() const noexcept {
            return active_;
        }

    private:
        friend class hpack_decoder;

        explicit decode_transaction_type(hpack_decoder& decoder) noexcept;

        hpack_decoder* decoder_;
        bool active_{true};
    };

    explicit hpack_decoder(hpack_decoder_options options = {});

    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;

    void set_max_dynamic_table_size(std::size_t bytes);
    [[nodiscard]] decode_transaction_type begin_transaction() noexcept;
    [[nodiscard]] hpack_decode_result decode(
        std::string_view block, void* target, header_callback_type callback);
    [[nodiscard]] hpack_decode_result decode(std::string_view block, void* target,
        header_callback_type callback, decode_transaction_type& transaction);

private:
    using step_result_type = std::optional<hpack_decode_error>;

    struct entry_type final {
        static constexpr std::size_t inline_capacity = 48;

        union storage_type final {
            constexpr storage_type() noexcept
                : inline_bytes_{} {}
            char inline_bytes_[inline_capacity];
            char* heap_;
        };

        entry_type() noexcept = default;
        entry_type(std::pmr::memory_resource* resource, std::string_view name, std::string_view value);
        entry_type(const entry_type&) = delete;
        entry_type& operator=(const entry_type&) = delete;
        entry_type(entry_type&& other) noexcept;
        entry_type& operator=(entry_type&&) = delete;
        ~entry_type();

        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] std::string_view value() const noexcept;
        [[nodiscard]] std::size_t table_size() const noexcept {
            return name_length_ + value_length_ + 32;
        }

    private:
        [[nodiscard]] bool is_inline() const noexcept {
            return name_length_ <= inline_capacity &&
                   value_length_ <= inline_capacity - name_length_;
        }
        void restore_inline_empty() noexcept;

        std::pmr::memory_resource* resource_{nullptr};
        std::size_t name_length_{0};
        std::size_t value_length_{0};
        storage_type storage_;
    };

    struct header_view_type final {
        std::string_view name_;
        std::string_view value_;
    };

    [[nodiscard]] hpack_decode_result decode_block(
        std::string_view block, void* target, header_callback_type callback);
    [[nodiscard]] step_result_type decode_integer(const unsigned char*& cursor, const unsigned char* end,
        std::uint8_t prefix_bits, std::uint32_t& value) const noexcept;
    [[nodiscard]] step_result_type decode_string(const unsigned char*& cursor, const unsigned char* end,
        std::pmr::string& scratch, std::string_view& value);
    // `rejected` is an in/out latch: once a callback has returned false, no further
    // callbacks fire, BUT the block keeps decoding and dynamic-table insertions still
    // apply (including this entry's) so the connection-global table stays consistent
    // -- RFC 7541 requires the whole field block to be processed. Set true here when a
    // fresh callback rejects; the caller reports it after finishing the block.
    [[nodiscard]] step_result_type decode_literal_header(const unsigned char*& cursor,
        const unsigned char* end, std::uint8_t name_index_prefix_bits, bool index_into_dynamic,
        void* target, header_callback_type callback, bool& rejected);
    void release_scratch();
    [[nodiscard]] step_result_type indexed_header(std::uint32_t index, header_view_type& header) const noexcept;
    [[nodiscard]] step_result_type indexed_name(
        std::uint32_t index, std::string_view& name) const noexcept;
    [[nodiscard]] std::size_t dynamic_entry_count() const noexcept;
    [[nodiscard]] const entry_type& dynamic_entry_by_newest_index(std::size_t newest_index) const noexcept;
    void add_dynamic(std::string_view name, std::string_view value);
    void clear_dynamic() noexcept;
    void evict_dynamic_to_fit(std::size_t entry_size);
    void evict_dynamic();
    void compact_dynamic() noexcept;
    void begin_decode_transaction() noexcept;
    void commit_decode_transaction() noexcept;
    void rollback_decode_transaction() noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<entry_type> dynamic_;
    std::pmr::string name_scratch_;
    std::pmr::string value_scratch_;
    std::size_t dynamic_size_{0};
    std::size_t dynamic_offset_{0};
    std::size_t max_dynamic_size_{4096};
    std::size_t allowed_dynamic_size_{4096};
    bool decode_transaction_active_{false};
    std::size_t transaction_dynamic_size_{0};
    std::size_t transaction_dynamic_offset_{0};
    std::size_t transaction_dynamic_vector_size_{0};
    std::size_t transaction_max_dynamic_size_{0};
};

class hpack_encoder final {
public:
    static void encode_indexed(std::pmr::string& out, std::uint32_t index);
    static void encode_dynamic_table_size_update(std::pmr::string& out, std::uint32_t maximum);
    // Throws std::length_error before changing `out` when a literal name or value
    // exceeds the uint32 length domain permitted by HPACK string literals.
    static void encode_header(std::pmr::string& out, std::string_view name, std::string_view value);
    // Throws std::length_error before changing `out` when `value` is too large for
    // the HPACK string length field.
    static void encode_header_with_name_index(std::pmr::string& out, std::uint32_t name_index,
        std::string_view value, bool never_indexed = false);
    static void encode_status(std::pmr::string& out, http_status_code status);

private:
    static void encode_integer(std::pmr::string& out, std::uint8_t first_byte_mask,
        std::uint8_t prefix_bits, std::uint32_t value);
    static void encode_string(std::pmr::string& out, std::string_view value);
};

}  // namespace ruvia::detail
