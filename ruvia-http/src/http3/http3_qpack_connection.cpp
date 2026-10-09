#include "ruvia/http/http3_qpack_connection.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/http3_qpack.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/qpack_static_table.h"

namespace ruvia {
namespace {
using error_type = http3_qpack_connection_error;
struct failure_guard {
    std::optional<error_type>& failure_;
    error_type error_;
    int exceptions_{std::uncaught_exceptions()};
    ~failure_guard() {
        if (std::uncaught_exceptions() > exceptions_) {
            failure_ = error_;
        }
    }
};
void integer(std::pmr::vector<char>& out, std::uint8_t bits, std::uint8_t flags, std::uint64_t value) {
    std::array<char, 11> data{};
    auto size = encode_http3_qpack_integer(data, bits, flags, value);
    if ((size.index() != 0)) {
        throw std::logic_error("QPACK integer encoding failed");
    }
    out.insert(out.end(), data.begin(), data.begin() + std::get<0>(size));
}
void literal(std::pmr::vector<char>& out, std::uint8_t bits, std::uint8_t flags, std::string_view text) {
    integer(out, bits, flags, text.size());
    out.insert(out.end(), text.begin(), text.end());
}
bool consume(std::pmr::vector<char>& out, std::size_t count) noexcept {
    if (count > out.size()) {
        return false;
    }
    out.erase(out.begin(), out.begin() + count);
    return true;
}
struct entry {
    std::pmr::string name_, value_;
    std::uint64_t absolute_;
    std::size_t references_{0};
    entry(std::string_view n, std::string_view v, std::uint64_t index, std::pmr::memory_resource* r)
        : name_(n, r),
          value_(v, r),
          absolute_(index) {}
    entry(entry&& other)
        : name_(std::move(other.name_), other.name_.get_allocator()),
          value_(std::move(other.value_), other.value_.get_allocator()),
          absolute_(other.absolute_),
          references_(other.references_) {}
    std::size_t size() const noexcept {
        return name_.size() + value_.size() + 32;
    }
};
struct table {
    std::pmr::deque<entry> entries_;
    std::size_t capacity_{0}, used_{0};
    std::uint64_t count_{0};
    std::uint64_t evictable_before_{http3_var_int_max};
    explicit table(std::pmr::memory_resource* r)
        : entries_(r) {}
    const entry* at(std::uint64_t index) const noexcept {
        if (entries_.empty() || index < entries_.front().absolute_ || index >= count_) {
            return nullptr;
        }
        return &entries_[static_cast<std::size_t>(index - entries_.front().absolute_)];
    }
    entry* at(std::uint64_t index) noexcept {
        return const_cast<entry*>(std::as_const(*this).at(index));
    }
    bool evict_to(std::size_t budget) {
        std::size_t removable = 0;
        for (const auto& e : entries_) {
            if (used_ - removable <= budget) {
                break;
            }
            if (e.references_ || e.absolute_ >= evictable_before_) {
                return false;
            }
            removable += e.size();
        }
        while (used_ > budget && !entries_.empty()) {
            used_ -= entries_.front().size();
            entries_.pop_front();
        }
        return used_ <= budget;
    }
    bool insert(std::string_view name, std::string_view value) {
        if (name.size() > capacity_ || value.size() > capacity_ - name.size() ||
            capacity_ - name.size() - value.size() < 32 || count_ == http3_var_int_max) {
            return false;
        }
        // Copy first: a duplicate may refer to the entry evicted by this insert.
        entry candidate_value(name, value, count_, entries_.get_allocator().resource());
        if (!evict_to(capacity_ - candidate_value.size())) {
            return false;
        }
        entries_.push_back(std::move(candidate_value));
        used_ += entries_.back().size();
        ++count_;
        return true;
    }
};
void static_literal(std::pmr::vector<char>& out, http3_field_section_field_view field,
    std::optional<detail::static_field_match> match) {
    if (match && match->exact_index_) {
        integer(out, 6, 0xc0, *match->exact_index_);
        return;
    }
    if (match) {
        integer(out, 4, field.never_indexed_ ? 0x70 : 0x50, match->name_index_);
    } else {
        literal(out, 3, field.never_indexed_ ? 0x30 : 0x20, field.name_);
    }
    literal(out, 7, 0, field.value_);
}
void validate_config(std::size_t capacity, std::size_t blocked) {
    if (capacity > http3_var_int_max || blocked > http3_var_int_max) {
        throw std::invalid_argument("QPACK limits exceed QUIC varint range");
    }
}
}  // namespace

struct http3_qpack_decoder::impl_type {
    http3_qpack_decoder_config config_;
    table table_;
    std::pmr::vector<char> input_, output_;
    // Hash storage allocates debug proxies inside a noexcept constructor on
    // MSVC. Ordered maps propagate initialization failures to their caller.
    std::pmr::map<std::uint64_t, std::uint64_t> blocked_;
    std::optional<error_type> failure_;
    bool decoding_{false};
    impl_type(http3_qpack_decoder_config c, std::pmr::memory_resource* r)
        : config_(c),
          table_(r),
          input_(0, r),
          output_(0, r),
          blocked_(r) {}
    std::variant<bool, error_type> instruction() {
        if (input_.empty()) {
            return false;
        }
        auto bytes_value = std::span<const char>(input_);
        auto first = static_cast<std::uint8_t>(bytes_value[0]);
        auto index = decode_http3_qpack_integer(bytes_value, (first & 0x80) ? 6 : (first & 0x40) ? 5
                                                                                                 : 5);
        if ((index.index() != 0)) {
            return std::get<1>(index) == http3_qpack_error::need_more_data ? std::variant<bool, error_type>(false) : error_type::encoder_stream_error;
        }
        std::size_t offset = std::get<0>(index).encoded_bytes_;
        if ((first & 0xe0) == 0x20) {
            if (std::get<0>(index).value_ > config_.max_table_capacity_ || !table_.evict_to(static_cast<std::size_t>(std::get<0>(index).value_))) {
                return error_type::encoder_stream_error;
            }
            table_.capacity_ = static_cast<std::size_t>(std::get<0>(index).value_);
        } else if ((first & 0xe0) == 0) {
            if (std::get<0>(index).value_ >= table_.count_) {
                return error_type::encoder_stream_error;
            }
            auto entry_value = table_.at(table_.count_ - std::get<0>(index).value_ - 1);
            if (!entry_value || !table_.insert(entry_value->name_, entry_value->value_)) {
                return error_type::encoder_stream_error;
            }
        } else {
            // Wait for both literals before decoding either one: otherwise a
            // fragmented value repeatedly allocates and decodes its complete name.
            const auto bound = std::min(config_.max_table_capacity_, (std::numeric_limits<std::size_t>::max() - 64) / 4) * 4 + 64;
            if (!(first & 0x80)) {
                if (std::get<0>(index).value_ > bound) {
                    return error_type::encoder_stream_error;
                }
                if (std::get<0>(index).value_ > bytes_value.size() - offset) {
                    return false;
                }
                offset += static_cast<std::size_t>(std::get<0>(index).value_);
            }
            auto value_length = decode_http3_qpack_integer(bytes_value.subspan(offset), 7);
            if ((value_length.index() != 0)) {
                return std::get<1>(value_length) == http3_qpack_error::need_more_data
                           ? std::variant<bool, error_type>(false)
                           : error_type::encoder_stream_error;
            }
            if (std::get<0>(value_length).value_ > bound) {
                return error_type::encoder_stream_error;
            }
            if (std::get<0>(value_length).value_ > bytes_value.size() - offset - std::get<0>(value_length).encoded_bytes_) {
                return false;
            }
            offset = std::get<0>(index).encoded_bytes_;
            std::pmr::string name(0, '\0', input_.get_allocator().resource());
            std::pmr::string value(0, '\0', input_.get_allocator().resource());
            if (first & 0x80) {
                if (first & 0x40) {
                    auto e = get_http3_qpack_static_entry(std::get<0>(index).value_);
                    if ((e.index() != 0)) {
                        return error_type::encoder_stream_error;
                    }
                    name = std::get<0>(e).name_;
                } else {
                    if (std::get<0>(index).value_ >= table_.count_) {
                        return error_type::encoder_stream_error;
                    }
                    auto e = table_.at(table_.count_ - std::get<0>(index).value_ - 1);
                    if (!e) {
                        return error_type::encoder_stream_error;
                    }
                    name = e->name_;
                }
            } else {
                auto size = decode_http3_qpack_string(bytes_value, 5, name);
                if ((size.index() != 0)) {
                    return std::get<1>(size) == http3_qpack_error::need_more_data ? std::variant<bool, error_type>(false) : error_type::encoder_stream_error;
                }
                offset = std::get<0>(size);
            }
            auto size = decode_http3_qpack_string(bytes_value.subspan(offset), value);
            if ((size.index() != 0)) {
                return std::get<1>(size) == http3_qpack_error::need_more_data ? std::variant<bool, error_type>(false) : error_type::encoder_stream_error;
            }
            offset += std::get<0>(size);
            if (!table_.insert(name, value)) {
                return error_type::encoder_stream_error;
            }
        }
        input_.erase(input_.begin(), input_.begin() + offset);
        return true;
    }
};
http3_qpack_decoder::http3_qpack_decoder(http3_qpack_decoder_config config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      impl_(nullptr) {
    validate_config(config.max_table_capacity_, config.max_blocked_streams_);
    std::pmr::polymorphic_allocator<impl_type> a(resource_);
    impl_ = a.new_object<impl_type>(config, resource_);
}
http3_qpack_decoder::~http3_qpack_decoder() {
    std::pmr::polymorphic_allocator<impl_type>(resource_).delete_object(impl_);
}
std::variant<std::monostate, error_type> http3_qpack_decoder::consume_encoder(std::span<const char> bytes_value, bool fin) {
    auto& s = *impl_;
    if (s.decoding_) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure_) {
        return *s.failure_;
    }
    if (fin) {
        s.failure_ = error_type::closed_critical_stream;
        return *s.failure_;
    }
    failure_guard guard_value{s.failure_, error_type::encoder_stream_error};
    auto before = s.table_.count_;
    // Feed one byte at a time so a claimed literal length cannot cause an
    // unbounded instruction buffer. Large batches contain arbitrarily many instructions.
    for (char byte : bytes_value) {
        if (s.input_.size() >= std::min(s.config_.max_table_capacity_, (std::numeric_limits<std::size_t>::max() - 64) / 4) * 4 + 64) {
            s.failure_ = error_type::encoder_stream_error;
            return *s.failure_;
        }
        s.input_.push_back(byte);
        auto result_value = s.instruction();
        if ((result_value.index() != 0)) {
            s.failure_ = std::get<1>(result_value);
            return *s.failure_;
        }
    }
    if (s.table_.count_ > before) {
        if (s.output_.size() > s.config_.max_pending_output_bytes_ || s.config_.max_pending_output_bytes_ - s.output_.size() < 11) {
            s.failure_ = error_type::limit;
            return *s.failure_;
        }
        integer(s.output_, 6, 0, s.table_.count_ - before);
    }
    std::erase_if(s.blocked_, [&](const auto& item) { return item.second <= s.table_.count_; });
    return {};
}
std::variant<http3_qpack_decode_result, error_type> http3_qpack_decoder::decode(std::uint64_t stream_id, std::span<const char> section,
    http3_field_section_callback_type callback_value, void* context_value) {
    auto& s = *impl_;
    if (s.decoding_) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure_) {
        return *s.failure_;
    }
    if (stream_id > http3_var_int_max) {
        return error_type::invalid_stream_id;
    }
    auto fail = [&](error_type e) -> std::variant<http3_qpack_decode_result, error_type> {s.failure_=e;return e; };
    if (section.size() > s.config_.fields_.max_encoded_bytes_) {
        return fail(error_type::limit);
    }
    failure_guard exception_guard{s.failure_, error_type::decompression_failed};
    auto encoded = decode_http3_qpack_integer(section, 8);
    if ((encoded.index() != 0) || std::get<0>(encoded).encoded_bytes_ >= section.size()) {
        return fail(error_type::decompression_failed);
    }
    auto delta = decode_http3_qpack_integer(section.subspan(std::get<0>(encoded).encoded_bytes_), 7);
    if ((delta.index() != 0)) {
        return fail(error_type::decompression_failed);
    }
    std::uint64_t required = 0;
    if (std::get<0>(encoded).value_) {
        auto max_entries = s.config_.max_table_capacity_ / 32;
        auto full_range = 2 * max_entries;
        if (!full_range || std::get<0>(encoded).value_ > full_range) {
            return fail(error_type::decompression_failed);
        }
        auto max_value = s.table_.count_ + max_entries;
        required = (max_value / full_range) * full_range + std::get<0>(encoded).value_ - 1;
        if (required > max_value) {
            if (required <= full_range) {
                return fail(error_type::decompression_failed);
            }
            required -= full_range;
        }
        if (!required) {
            return fail(error_type::decompression_failed);
        }
    }
    bool negative = (static_cast<std::uint8_t>(section[std::get<0>(encoded).encoded_bytes_]) & 0x80) != 0;
    if ((negative && std::get<0>(delta).value_ >= required) || (!negative && std::get<0>(delta).value_ > http3_var_int_max - required)) {
        return fail(error_type::decompression_failed);
    }
    auto base = negative ? required - std::get<0>(delta).value_ - 1 : required + std::get<0>(delta).value_;
    if (required > s.table_.count_) {
        if (!s.blocked_.contains(stream_id) && s.blocked_.size() >= s.config_.max_blocked_streams_) {
            return fail(error_type::decompression_failed);
        }
        s.blocked_[stream_id] = required;
        return http3_qpack_decode_result{http3_qpack_decode_status::blocked, 0};
    }
    s.blocked_.erase(stream_id);
    struct guard {
        std::optional<error_type>& failure_;
        int exceptions_{std::uncaught_exceptions()};
        bool& value_;
        guard(bool& v, std::optional<error_type>& f)
            : failure_(f),
              value_(v) {
            value_ = true;
        }
        ~guard() {
            value_ = false;
            if (std::uncaught_exceptions() > exceptions_) {
                failure_ = error_type::decompression_failed;
            }
        }
    } guard_value(s.decoding_, s.failure_);
    std::size_t offset = std::get<0>(encoded).encoded_bytes_ + std::get<0>(delta).encoded_bytes_, count = 0, total = 0;
    bool callback_stopped = false;
    std::uint64_t highest = 0;
    std::pmr::string name(0, '\0', resource_);
    std::pmr::string value(0, '\0', resource_);
    auto dynamic = [&](std::uint64_t index, bool post) -> const entry* {
        if ((post && index > http3_var_int_max - base) || (!post && index >= base)) {
            return nullptr;
        }
        auto absolute = post ? base + index : base - index - 1;
        if (absolute >= required) {
            return nullptr;
        }
        auto e = s.table_.at(absolute);
        if (e) {
            highest = std::max(highest, absolute + 1);
        }
        return e;
    };
    while (offset < section.size()) {
        auto bytes_value = section.subspan(offset);
        auto first = static_cast<std::uint8_t>(bytes_value[0]);
        http3_field_section_field_view field{};
        bool indexed = (first & 0x80) != 0, name_reference = (first & 0xc0) == 0x40,
             literal_name = (first & 0xe0) == 0x20, post_indexed = (first & 0xf0) == 0x10;
        if (indexed || name_reference || post_indexed || !literal_name) {
            auto index = decode_http3_qpack_integer(bytes_value, indexed ? 6 : name_reference ? 4
                                                                           : post_indexed     ? 4
                                                                                              : 3);
            if ((index.index() != 0)) {
                return fail(error_type::decompression_failed);
            }
            offset += std::get<0>(index).encoded_bytes_;
            bool is_static = indexed ? (first & 0x40) != 0 : name_reference && (first & 0x10) != 0;
            if (is_static) {
                auto e = get_http3_qpack_static_entry(std::get<0>(index).value_);
                if ((e.index() != 0)) {
                    return fail(error_type::decompression_failed);
                }
                field = {std::get<0>(e).name_, std::get<0>(e).value_, false};
            } else {
                auto e = dynamic(std::get<0>(index).value_, post_indexed || (!indexed && !name_reference));
                if (!e) {
                    return fail(error_type::decompression_failed);
                }
                field = {e->name_, e->value_, false};
            }
            if (!indexed && !post_indexed) {
                auto consumed = decode_http3_qpack_string(section.subspan(offset), value);
                if ((consumed.index() != 0)) {
                    return fail(error_type::decompression_failed);
                }
                offset += std::get<0>(consumed);
                field.value_ = value;
                field.never_indexed_ = (first & (name_reference ? 0x20 : 0x08)) != 0;
            }
        } else {
            auto n = decode_http3_qpack_string(bytes_value, 3, name);
            if ((n.index() != 0)) {
                return fail(error_type::decompression_failed);
            }
            offset += std::get<0>(n);
            auto v = decode_http3_qpack_string(section.subspan(offset), value);
            if ((v.index() != 0)) {
                return fail(error_type::decompression_failed);
            }
            offset += std::get<0>(v);
            field = {name, value, (first & 0x10) != 0};
        }
        if (count >= s.config_.fields_.max_fields_ || total > s.config_.fields_.max_decoded_bytes_ ||
            s.config_.fields_.max_decoded_bytes_ - total < 32 || field.name_.size() > s.config_.fields_.max_decoded_bytes_ - total - 32 ||
            field.value_.size() > s.config_.fields_.max_decoded_bytes_ - total - 32 - field.name_.size()) {
            return fail(error_type::limit);
        }
        total += 32 + field.name_.size() + field.value_.size();
        ++count;
        if (callback_value && !callback_stopped && !callback_value(context_value, field)) {
            callback_stopped = true;
        }
    }
    if (highest > required) {
        return fail(error_type::decompression_failed);
    }
    if (required) {
        if (s.output_.size() > s.config_.max_pending_output_bytes_ || s.config_.max_pending_output_bytes_ - s.output_.size() < 11) {
            return fail(error_type::limit);
        }
        integer(s.output_, 7, 0x80, stream_id);
    }
    return http3_qpack_decode_result{callback_stopped ? http3_qpack_decode_status::callback_stopped : http3_qpack_decode_status::decoded, count};
}
std::variant<std::monostate, error_type> http3_qpack_decoder::cancel(std::uint64_t stream_id) {
    auto& s = *impl_;
    if (s.decoding_) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure_) {
        return *s.failure_;
    }
    if (stream_id > http3_var_int_max) {
        return error_type::invalid_stream_id;
    }
    s.blocked_.erase(stream_id);
    if (s.config_.max_table_capacity_ == 0) {
        return {};
    }
    if (s.output_.size() > s.config_.max_pending_output_bytes_ || s.config_.max_pending_output_bytes_ - s.output_.size() < 11) {
        s.failure_ = error_type::limit;
        return *s.failure_;
    }
    failure_guard guard_value{s.failure_, error_type::decompression_failed};
    integer(s.output_, 6, 0x40, stream_id);
    return {};
}
std::span<const char> http3_qpack_decoder::pending_decoder_output() const& noexcept {
    return impl_->output_;
}
bool http3_qpack_decoder::consume_decoder_output(std::size_t bytes_value) noexcept {
    return !impl_->decoding_ && consume(impl_->output_, bytes_value);
}
std::uint64_t http3_qpack_decoder::insert_count() const noexcept {
    return impl_->table_.count_;
}
std::size_t http3_qpack_decoder::blocked_stream_count() const noexcept {
    return impl_->blocked_.size();
}

struct http3_qpack_encoder::impl_type {
    struct section_type {
        std::uint64_t required_;
        std::pmr::vector<std::uint64_t> references_;
        section_type(std::uint64_t ric, std::pmr::vector<std::uint64_t>&& refs)
            : required_(ric),
              references_(std::move(refs), refs.get_allocator()) {}
    };
    http3_qpack_encoder_config config_;
    table table_;
    std::pmr::vector<char> input_, output_;
    std::pmr::map<std::uint64_t, std::pmr::deque<section_type>> sections_;
    std::uint64_t received_{0};
    std::size_t outstanding_sections_{0};
    std::optional<error_type> failure_;
    impl_type(http3_qpack_encoder_config c, std::pmr::memory_resource* r)
        : config_(c),
          table_(r),
          input_(0, r),
          output_(0, r),
          sections_(r) {
        table_.capacity_ = c.table_capacity_.value_or(c.max_table_capacity_);
        table_.evictable_before_ = 0;
        if (table_.capacity_) {
            integer(output_, 5, 0x20, table_.capacity_);
        }
    }
    void release(const section_type& section) {
        for (auto absolute : section.references_) {
            auto e = table_.at(absolute);
            if (e && e->references_) {
                --e->references_;
            }
        }
    }
    std::size_t blocked_count() const {
        std::size_t n = 0;
        for (const auto& [id, list] : sections_) {
            (void)id;
            if (std::any_of(list.begin(), list.end(), [&](const auto& section) { return section.required_ > received_; })) {
                ++n;
            }
        }
        return n;
    }
};
http3_qpack_encoder::http3_qpack_encoder(http3_qpack_encoder_config config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      impl_(nullptr) {
    validate_config(config.max_table_capacity_, config.max_blocked_streams_);
    if ((config.table_capacity_ && *config.table_capacity_ > config.max_table_capacity_) ||
        config.max_pending_output_bytes_ < 11) {
        throw std::invalid_argument("invalid QPACK encoder limits");
    }
    impl_ = std::pmr::polymorphic_allocator<impl_type>(resource_).new_object<impl_type>(config, resource_);
}
http3_qpack_encoder::~http3_qpack_encoder() {
    std::pmr::polymorphic_allocator<impl_type>(resource_).delete_object(impl_);
}
std::variant<std::pmr::vector<char>, error_type> http3_qpack_encoder::encode(std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits, std::pmr::memory_resource* result_resource) {
    auto& s = *impl_;
    if (s.failure_) {
        return *s.failure_;
    }
    if (stream_id > http3_var_int_max) {
        return error_type::invalid_stream_id;
    }
    limits.max_fields_ = std::min(limits.max_fields_, s.config_.fields_.max_fields_);
    limits.max_decoded_bytes_ = std::min(limits.max_decoded_bytes_, s.config_.fields_.max_decoded_bytes_);
    limits.max_encoded_bytes_ = std::min(limits.max_encoded_bytes_, s.config_.fields_.max_encoded_bytes_);
    std::size_t total = 0;
    if (fields_value.size() > limits.max_fields_) {
        return error_type::limit;
    }
    for (const auto& f : fields_value) {
        if (total > limits.max_decoded_bytes_ || limits.max_decoded_bytes_ - total < 32 ||
            f.name_.size() > limits.max_decoded_bytes_ - total - 32 ||
            f.value_.size() > limits.max_decoded_bytes_ - total - 32 - f.name_.size()) {
            return error_type::limit;
        }
        total += 32 + f.name_.size() + f.value_.size();
    }
    failure_guard exception_guard{s.failure_, error_type::decoder_stream_error};
    auto* output_resource = result_resource ? result_resource : resource_;
    auto existing = s.sections_.find(stream_id);
    bool already_blocked = existing != s.sections_.end() && std::any_of(existing->second.begin(), existing->second.end(), [&](const auto& section) { return section.required_ > s.received_; });
    const bool may_reference = s.outstanding_sections_ < s.config_.max_outstanding_sections_;
    bool may_block = may_reference && (already_blocked || s.blocked_count() < s.config_.max_blocked_streams_);
    // Empty containers can allocate debug proxies. Size constructors let those
    // failures propagate through the guard and latch the terminal QPACK error.
    std::pmr::vector<char> body(0, resource_);
    std::pmr::vector<std::uint64_t> references(0, resource_);
    std::uint64_t required = 0;
    for (const auto& f : fields_value) {
        const auto static_match = detail::qpack_static_fields.find(f.name_,
            f.never_indexed_ ? std::nullopt : std::optional(f.value_));
        if (!may_reference || f.never_indexed_ || (static_match && static_match->exact_index_)) {
            static_literal(body, f, static_match);
            continue;
        }
        entry* selected = nullptr;
        for (auto& e : s.table_.entries_) {
            if (e.name_ == f.name_ && e.value_ == f.value_ && (may_block || e.absolute_ < s.received_)) {
                selected = &e;
                break;
            }
        }
        const auto instruction_limit = s.config_.max_pending_output_bytes_;
        const bool can_insert = s.output_.size() <= instruction_limit && instruction_limit - s.output_.size() >= 22 &&
                                f.name_.size() <= instruction_limit - s.output_.size() - 22 &&
                                f.value_.size() <= instruction_limit - s.output_.size() - 22 - f.name_.size();
        if (!selected && may_block && can_insert && s.table_.insert(f.name_, f.value_)) {
            literal(s.output_, 5, 0x40, f.name_);
            literal(s.output_, 7, 0, f.value_);
            selected = &s.table_.entries_.back();
        }
        if (!selected) {
            static_literal(body, f, static_match);
            continue;
        }
        // Base is zero, so all dynamic references use post-base indexing.
        integer(body, 4, 0x10, selected->absolute_);
        required = std::max(required, selected->absolute_ + 1);
        references.push_back(selected->absolute_);
        ++selected->references_;
    }
    std::array<char, 22> prefix;
    const auto insert_count_size = encode_http3_qpack_integer(prefix, 8, 0,
        required ? required % (2 * (s.config_.max_table_capacity_ / 32)) + 1 : 0);
    if (insert_count_size.index() != 0) {
        throw std::logic_error("QPACK field section prefix encoding failed");
    }
    const auto base_size = encode_http3_qpack_integer(std::span(prefix).subspan(std::get<0>(insert_count_size)),
        7, required ? 0x80 : 0, required ? required - 1 : 0);
    if (base_size.index() != 0) {
        throw std::logic_error("QPACK field section prefix encoding failed");
    }
    const auto prefix_size = std::get<0>(insert_count_size) + std::get<0>(base_size);
    std::pmr::vector<char> output(0, output_resource);
    if (body.size() > output.max_size() - prefix_size) {
        throw std::length_error("QPACK field section exceeds the output size limit");
    }
    output.reserve(prefix_size + body.size());
    output.insert(output.end(), prefix.begin(), prefix.begin() + static_cast<std::ptrdiff_t>(prefix_size));
    output.insert(output.end(), body.begin(), body.end());
    if (output.size() > limits.max_encoded_bytes_) {
        for (auto index : references) {
            --s.table_.at(index)->references_;
        }
        return error_type::limit;
    }
    if (required) {
        s.sections_[stream_id].emplace_back(required, std::move(references));
        ++s.outstanding_sections_;
    }
    return std::variant<std::pmr::vector<char>, error_type>(std::in_place_index<0>, std::move(output), output.get_allocator());
}
std::variant<std::monostate, error_type> http3_qpack_encoder::consume_decoder(std::span<const char> bytes_value, bool fin) {
    auto& s = *impl_;
    if (s.failure_) {
        return *s.failure_;
    }
    auto fail = [&](error_type e) -> std::variant<std::monostate, error_type> {s.failure_=e;return e; };
    if (fin) {
        return fail(error_type::closed_critical_stream);
    }
    failure_guard exception_guard{s.failure_, error_type::decoder_stream_error};
    for (char byte : bytes_value) {
        if (s.input_.size() >= 11) {
            return fail(error_type::decoder_stream_error);
        }
        s.input_.push_back(byte);
        auto first = static_cast<std::uint8_t>(s.input_[0]);
        auto decoded = decode_http3_qpack_integer(s.input_, (first & 0x80) ? 7 : 6);
        if ((decoded.index() != 0)) {
            if (std::get<1>(decoded) == http3_qpack_error::need_more_data) {
                continue;
            }
            return fail(error_type::decoder_stream_error);
        }
        auto value = std::get<0>(decoded).value_;
        if (first & 0x80) {
            auto found = s.sections_.find(value);
            if (found == s.sections_.end() || found->second.empty()) {
                return fail(error_type::decoder_stream_error);
            }
            auto& section = found->second.front();
            s.received_ = std::max(s.received_, section.required_);
            s.release(section);
            found->second.pop_front();
            --s.outstanding_sections_;
            if (found->second.empty()) {
                s.sections_.erase(found);
            }
        } else if (first & 0x40) {
            auto found = s.sections_.find(value);
            if (found != s.sections_.end()) {
                s.outstanding_sections_ -= found->second.size();
                for (auto& section : found->second) {
                    s.release(section);
                }
                s.sections_.erase(found);
            }
        } else {
            if (!value || value > s.table_.count_ - s.received_) {
                return fail(error_type::decoder_stream_error);
            }
            s.received_ += value;
        }
        s.table_.evictable_before_ = s.received_;
        s.input_.clear();
    }
    return {};
}
std::span<const char> http3_qpack_encoder::pending_encoder_output() const& noexcept {
    return impl_->output_;
}
bool http3_qpack_encoder::consume_encoder_output(std::size_t bytes_value) noexcept {
    return consume(impl_->output_, bytes_value);
}
std::uint64_t http3_qpack_encoder::insert_count() const noexcept {
    return impl_->table_.count_;
}
std::uint64_t http3_qpack_encoder::known_received_count() const noexcept {
    return impl_->received_;
}
}  // namespace ruvia
