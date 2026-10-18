#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "http2/http2_hpack.h"
#include "http2/http2_hpack_static_table.h"

namespace ruvia::detail {

hpack_decoder::entry_type::entry_type(
    std::pmr::memory_resource* owner_value, std::string_view entry_name, std::string_view entry_value)
    : resource_(owner_value),
      name_length_(entry_name.size()),
      value_length_(entry_value.size()) {
    const auto length = name_length_ + value_length_;
    if (is_inline()) {
        if (name_length_ != 0) {
            std::memcpy(storage_.inline_bytes_, entry_name.data(), name_length_);
        }
        if (value_length_ != 0) {
            std::memcpy(storage_.inline_bytes_ + name_length_, entry_value.data(), value_length_);
        }
        return;
    }

    auto allocator = std::pmr::polymorphic_allocator<char>(resource_);
    char* bytes_value = allocator.allocate(length);
    storage_.heap_ = bytes_value;
    if (name_length_ != 0) {
        std::memcpy(bytes_value, entry_name.data(), name_length_);
    }
    if (value_length_ != 0) {
        std::memcpy(bytes_value + name_length_, entry_value.data(), value_length_);
    }
}

hpack_decoder::entry_type::entry_type(entry_type&& other) noexcept
    : resource_(other.resource_),
      name_length_(other.name_length_),
      value_length_(other.value_length_) {
    if (other.is_inline()) {
        const auto length = name_length_ + value_length_;
        if (length != 0) {
            std::memcpy(storage_.inline_bytes_, other.storage_.inline_bytes_, length);
        }
    } else {
        storage_.heap_ = other.storage_.heap_;
    }
    other.restore_inline_empty();
}

hpack_decoder::entry_type::~entry_type() {
    if (!is_inline()) {
        auto allocator = std::pmr::polymorphic_allocator<char>(resource_);
        allocator.deallocate(storage_.heap_, name_length_ + value_length_);
    }
}

std::string_view hpack_decoder::entry_type::name() const noexcept {
    const char* bytes_value = is_inline() ? storage_.inline_bytes_ : storage_.heap_;
    return {bytes_value, name_length_};
}

std::string_view hpack_decoder::entry_type::value() const noexcept {
    const char* bytes_value = is_inline() ? storage_.inline_bytes_ : storage_.heap_;
    return {bytes_value + name_length_, value_length_};
}

void hpack_decoder::entry_type::restore_inline_empty() noexcept {
    const bool was_inline = is_inline();
    name_length_ = 0;
    value_length_ = 0;
    if (!was_inline) {
        // Built-in assignment starts the inline array's lifetime after a heap move.
        storage_.inline_bytes_[0] = '\0';
    }
}

hpack_decoder::step_result_type hpack_decoder::indexed_header(
    std::uint32_t index, header_view_type& header_value) const noexcept {
    if (index == 0) {
        return hpack_decode_error::invalid_index;
    }
    if (index <= hpack_static_table_size) {
        const auto& entry_value = hpack_static_header_at(index);
        header_value = header_view_type{entry_value.name_, entry_value.value_};
        return std::nullopt;
    }
    const auto dynamic_index = index - static_cast<std::uint32_t>(hpack_static_table_size);
    if (dynamic_index == 0 || dynamic_index > dynamic_entry_count()) {
        return hpack_decode_error::invalid_index;
    }
    const auto& entry_value = dynamic_entry_by_newest_index(static_cast<std::size_t>(dynamic_index - 1));
    header_value = header_view_type{entry_value.name(), entry_value.value()};
    return std::nullopt;
}

hpack_decoder::step_result_type hpack_decoder::indexed_name(
    std::uint32_t index, std::string_view& name) const noexcept {
    header_view_type header;
    if (const auto error = indexed_header(index, header); (error.has_value())) {
        return error;
    }
    name = header.name_;
    return std::nullopt;
}

void hpack_decoder::add_dynamic(std::string_view name, std::string_view value) {
    // Check against the table budget using subtraction before adding untrusted
    // lengths, avoiding overflow while preserving the oversized-entry clear rule.
    if (max_dynamic_size_ < 32) {
        clear_dynamic();
        return;
    }
    const auto field_budget = max_dynamic_size_ - 32;
    if (name.size() > field_budget || value.size() > field_budget - name.size()) {
        clear_dynamic();
        return;
    }
    const auto size = name.size() + value.size() + 32;

    // Copy name and value into owned storage before ANY operation that can move or
    // destroy a dynamic entry. For a "Literal
    // Header Field with Incremental Indexing -- Indexed Name" whose name indexes a
    // dynamic entry (RFC 7541 6.2.1), `name` aliases storage inside that entry.
    // RFC 7541 4.4 allows the insertion to evict the referenced entry, while a
    // vector reserve can move the entry's inline bytes even before eviction. Either
    // operation would invalidate the view. Materializing into packed owned storage
    // first makes both safe; if either allocation throws, the live table remains
    // untouched and the complete field block is retryable.
    entry_type entry(resource_, name, value);

    // Reserve before eviction so push_back cannot throw after logical entries have
    // been removed from the table. Grow geometrically rather than forcing an exact
    // reallocation for every insertion.
    if (dynamic_.size() == dynamic_.capacity()) {
        const auto max_size = dynamic_.max_size();
        if (dynamic_.size() == max_size) {
            throw std::length_error("HPACK dynamic table is too large");
        }
        const auto required = dynamic_.size() + 1;
        const auto doubled = dynamic_.capacity() > max_size / 2
                                 ? max_size
                                 : dynamic_.capacity() * 2;
        dynamic_.reserve(std::max(required, doubled));
    }
    evict_dynamic_to_fit(size);
    dynamic_.push_back(std::move(entry));
    dynamic_size_ += size;
}

std::size_t hpack_decoder::dynamic_entry_count() const noexcept {
    return dynamic_.size() - dynamic_offset_;
}

const hpack_decoder::entry_type& hpack_decoder::dynamic_entry_by_newest_index(
    std::size_t newest_index) const noexcept {
    return dynamic_[dynamic_.size() - newest_index - 1];
}

void hpack_decoder::clear_dynamic() noexcept {
    if (decode_transaction_active_) {
        // Preserve the pre-block snapshot for rollback, but reclaim entries
        // inserted and subsequently evicted within this block.
        const auto previous_offset = dynamic_offset_;
        dynamic_offset_ = dynamic_.size();
        dynamic_size_ = 0;
        retire_dynamic_prefix(previous_offset);
        return;
    }
    dynamic_.clear();
    dynamic_size_ = 0;
    dynamic_offset_ = 0;
}

void hpack_decoder::evict_dynamic_to_fit(std::size_t entry_size) {
    const auto previous_offset = dynamic_offset_;
    const auto target_size = max_dynamic_size_ - entry_size;
    while (dynamic_size_ > target_size && dynamic_offset_ < dynamic_.size()) {
        const auto& entry_value = dynamic_[dynamic_offset_++];
        dynamic_size_ -= entry_value.table_size();
    }
    retire_dynamic_prefix(previous_offset);
}

void hpack_decoder::retire_dynamic_prefix(std::size_t begin) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<entry_type>);
    static_assert(std::is_nothrow_move_constructible_v<entry_type>);
    const auto retained_prefix = decode_transaction_active_ ? transaction_dynamic_vector_size_ : 0;
    if (dynamic_offset_ <= retained_prefix) {
        return;
    }
    const auto remaining = dynamic_entry_count();
    if (dynamic_offset_ - retained_prefix < remaining) {
        // Return newly evicted heap fields without moving the live suffix.
        for (auto index = std::max(begin, retained_prefix); index < dynamic_offset_; ++index) {
            auto* entry = std::addressof(dynamic_[index]);
            std::destroy_at(entry);
            std::construct_at(entry);
        }
        return;
    }
    // The dead prefix covers every destination. Moving now is amortized over
    // at least as many evictions as survivors; no source overlaps a destination.
    for (std::size_t i = 0; i < remaining; ++i) {
        auto* destination = std::addressof(dynamic_[retained_prefix + i]);
        std::destroy_at(destination);
        std::construct_at(destination, std::move(dynamic_[dynamic_offset_ + i]));
    }
    // Also destroys evicted fields in the gap and the moved-from source suffix.
    dynamic_.resize(retained_prefix + remaining);
    dynamic_offset_ = retained_prefix;
}

void hpack_decoder::begin_decode_transaction() noexcept {
    if (decode_transaction_active_) {
        std::terminate();
    }
    // Prior commits already retired the prefix. Its empty slots remain in the
    // snapshot so rollback only drops appended entries and restores the offset.
    decode_transaction_active_ = true;
    transaction_dynamic_size_ = dynamic_size_;
    transaction_dynamic_offset_ = dynamic_offset_;
    transaction_dynamic_vector_size_ = dynamic_.size();
    transaction_max_dynamic_size_ = max_dynamic_size_;
}

void hpack_decoder::commit_decode_transaction() noexcept {
    if (!decode_transaction_active_) {
        std::terminate();
    }
    // Commit releases newly evicted fields immediately, but leaves empty slots
    // until compaction is amortized over at least as many evictions as survivors.
    decode_transaction_active_ = false;
    retire_dynamic_prefix(transaction_dynamic_offset_);
}

void hpack_decoder::rollback_decode_transaction() noexcept {
    if (!decode_transaction_active_) {
        return;
    }
    if (dynamic_.size() < transaction_dynamic_vector_size_) {
        std::terminate();
    }
    dynamic_.resize(transaction_dynamic_vector_size_);
    dynamic_size_ = transaction_dynamic_size_;
    dynamic_offset_ = transaction_dynamic_offset_;
    max_dynamic_size_ = transaction_max_dynamic_size_;
    decode_transaction_active_ = false;
}

}  // namespace ruvia::detail
