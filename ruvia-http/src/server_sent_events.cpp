#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "http_response_server.h"

namespace ruvia {
namespace {

// Splits `data` at CR, LF or CRLF and passes each line to `visitor`; the
// sizing pass and the encoder share this one line rule.
template <typename visitor_type>
void visit_sse_data_lines(std::string_view data, visitor_type&& visitor) {
    while (true) {
        const auto next_value = data.find_first_of("\r\n");
        visitor(next_value == std::string_view::npos ? data : data.substr(0, next_value));
        if (next_value == std::string_view::npos) {
            return;
        }
        auto advance = next_value + 1U;
        if (data[next_value] == '\r' && next_value + 1U < data.size() && data[next_value + 1U] == '\n') {
            advance = next_value + 2U;
        }
        data.remove_prefix(advance);
    }
}

constexpr std::string_view sse_utf8_bom = "\xEF\xBB\xBF";
constexpr std::string_view sse_replacement_character = "\xEF\xBF\xBD";
constexpr std::string_view sse_default_event_type = "message";

// The event stream is decoded with WHATWG UTF-8 decode, which replaces each
// maximal subpart of an ill-formed sequence with one U+FFFD. The decoder never
// consumes an ASCII byte, so repairing each field value after the line split
// matches decoding the stream first.
void replace_ill_formed_utf8(std::string& value) {
    const std::string_view bytes(value);
    std::size_t index = 0;
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        if (!sequence.well_formed_) {
            break;
        }
        index += sequence.length_;
    }
    if (index == bytes.size()) {
        return;
    }
    std::string repaired;
    repaired.reserve(bytes.size());
    repaired.append(bytes.substr(0, index));
    while (index < bytes.size()) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            repaired.push_back(bytes[index]);
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        repaired.append(sequence.well_formed_ ? bytes.substr(index, sequence.length_) : sse_replacement_character);
        index += sequence.length_;
    }
    value.swap(repaired);
}

}  // namespace

sse_parser::sse_parser(sse_parser_options options) :
    event_limit_(options.event_limit_) {}

sse_parse_result sse_parser::parse(std::string_view input) & {
    if (state_ != sse_parser_state::active) {
        throw std::logic_error("SSE parser is not active");
    }
    release_event_buffers();
    std::size_t position = 0;
    if (!bom_settled_) {
        while (position < input.size() && bom_matched_ < sse_utf8_bom.size() && input[position] == sse_utf8_bom[bom_matched_]) {
            ++bom_matched_;
            ++position;
        }
        if (bom_matched_ < sse_utf8_bom.size() && position == input.size()) {
            return {.consumed_bytes_ = position};
        }
        bom_settled_ = true;
        if (bom_matched_ < sse_utf8_bom.size()) {
            // Not a BOM: the bytes matched so far begin the first field name.
            append_name(sse_utf8_bom.substr(0, bom_matched_));
        }
    }
    while (position < input.size()) {
        if (std::exchange(skip_line_feed_, false) && input[position] == '\n') {
            ++position;
            continue;
        }
        const auto rest = input.substr(position);
        if (phase_ == line_phase::value_start) {
            phase_ = line_phase::value;
            if (rest.front() == ' ') {
                ++position;
            }
            continue;
        }
        if (phase_ == line_phase::name) {
            const auto stop = rest.find_first_of(":\r\n");
            append_name(rest.substr(0, stop));
            if (stop == std::string_view::npos) {
                return {.consumed_bytes_ = input.size()};
            }
            position += stop + 1;
            const bool unnamed = name_size_ == 0 && !name_overflow_;
            if (!unnamed) {
                classify_field();
            }
            if (rest[stop] == ':') {
                phase_ = unnamed ? line_phase::comment : line_phase::value_start;
                continue;
            }
            skip_line_feed_ = rest[stop] == '\r';
            if (unnamed) {
                if (dispatch()) {
                    return {.consumed_bytes_ = position, .event_ = sse_event(event_type_.empty() ? sse_default_event_type : std::string_view(event_type_), data_, last_event_id_)};
                }
                continue;
            }
            // A line without a colon names a field with an empty value.
            if (!end_line()) {
                fail();
                return {.consumed_bytes_ = position};
            }
            continue;
        }
        const auto stop = rest.find_first_of("\r\n");
        const auto bytes = rest.substr(0, stop);
        if (phase_ == line_phase::value && !append_value(bytes)) {
            fail();
            return {.consumed_bytes_ = position};
        }
        if (stop == std::string_view::npos) {
            return {.consumed_bytes_ = input.size()};
        }
        position += stop + 1;
        skip_line_feed_ = rest[stop] == '\r';
        if (phase_ == line_phase::comment) {
            reset_line();
        } else if (!end_line()) {
            fail();
            return {.consumed_bytes_ = position};
        }
    }
    return {.consumed_bytes_ = position};
}

void sse_parser::finish() noexcept {
    if (state_ != sse_parser_state::active) {
        return;
    }
    release_event_buffers();
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::finished;
}

void sse_parser::reset() noexcept {
    event_published_ = false;
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    http_clear_string_retaining_small(last_event_id_buffer_);
    http_clear_string_retaining_small(last_event_id_);
    reconnection_time_.reset();
    skip_line_feed_ = false;
    bom_matched_ = 0;
    bom_settled_ = false;
    state_ = sse_parser_state::active;
}

std::size_t sse_parser::buffered_bytes() const noexcept {
    return data_.size() + event_type_.size() + value_.size();
}

void sse_parser::append_name(std::string_view bytes) noexcept {
    if (name_overflow_ || bytes.size() > name_.size() - name_size_) {
        name_overflow_ = name_overflow_ || !bytes.empty();
        return;
    }
    bytes.copy(name_.data() + name_size_, bytes.size());
    name_size_ = static_cast<std::uint8_t>(name_size_ + bytes.size());
}

void sse_parser::classify_field() noexcept {
    const std::string_view name(name_.data(), name_overflow_ ? 0 : name_size_);
    field_ = name == "data"
        ? field_kind::data
        : name == "event"
        ? field_kind::event
        : name == "id"
        ? field_kind::id
        : name == "retry"
        ? field_kind::retry
        : field_kind::ignored;
}

bool sse_parser::append_value(std::string_view bytes) {
    if (field_ == field_kind::retry) {
        // Only ASCII digits make a valid reconnection time; a value beyond
        // the duration range saturates.
        constexpr auto max_retry = static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)());
        for (const char byte : bytes) {
            if (byte < '0' || byte > '9') {
                retry_valid_ = false;
                break;
            }
            const auto digit = static_cast<std::uint64_t>(byte - '0');
            retry_digits_ = true;
            retry_value_ = retry_value_ > (max_retry - digit) / 10 ? max_retry : retry_value_ * 10 + digit;
        }
        return true;
    }
    if (field_ == field_kind::ignored || bytes.empty()) {
        return true;
    }
    if (event_limit_.addition_exceeds(buffered_bytes(), bytes.size())) {
        return false;
    }
    (field_ == field_kind::data ? data_ : value_).append(bytes);
    return true;
}

bool sse_parser::end_line() {
    switch (field_) {
        case field_kind::data:
            if (event_limit_.addition_exceeds(buffered_bytes(), 1)) {
                return false;
            }
            data_.push_back('\n');
            break;
        case field_kind::event:
            replace_ill_formed_utf8(value_);
            event_type_.swap(value_);
            break;
        case field_kind::id:
            // An id containing NUL is ignored.
            if (value_.find('\0') == std::string::npos) {
                replace_ill_formed_utf8(value_);
                last_event_id_buffer_.swap(value_);
            }
            break;
        case field_kind::retry:
            if (retry_digits_ && retry_valid_) {
                reconnection_time_ = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(retry_value_));
            }
            break;
        case field_kind::ignored:
            break;
    }
    reset_line();
    return true;
}

bool sse_parser::dispatch() {
    last_event_id_ = last_event_id_buffer_;
    if (data_.empty()) {
        http_clear_string_retaining_small(event_type_);
        return false;
    }
    data_.pop_back();
    replace_ill_formed_utf8(data_);
    event_published_ = true;
    return true;
}

void sse_parser::reset_line() noexcept {
    phase_ = line_phase::name;
    field_ = field_kind::ignored;
    name_size_ = 0;
    name_overflow_ = false;
    value_.clear();
    retry_value_ = 0;
    retry_digits_ = false;
    retry_valid_ = true;
}

void sse_parser::release_event_buffers() noexcept {
    if (std::exchange(event_published_, false)) {
        http_clear_string_retaining_small(data_);
        http_clear_string_retaining_small(event_type_);
    }
}

void sse_parser::fail() noexcept {
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(value_);
    state_ = sse_parser_state::failed;
}

std::string format_sse_message(const sse_message& message) {
    if (message.event_.view().find_first_of("\r\n") != std::string_view::npos || (message.id_.has_value() && message.id_->view().find_first_of("\r\n") != std::string_view::npos)) {
        throw std::invalid_argument("SSE event and id must not contain CR or LF");
    }
    if (message.id_.has_value() && (message.id_->view().find('\0') != std::string_view::npos)) {
        throw std::invalid_argument("SSE id must not contain a NUL character");
    }
    if (message.retry_.has_value() && message.retry_->count() < 0) {
        throw std::invalid_argument("SSE retry delay must not be negative");
    }
    // An event stream is UTF-8 (WHATWG HTML, "Parsing an event stream").
    if (!is_valid_utf8(message.event_.view()) || (message.id_.has_value() && !is_valid_utf8(message.id_->view())) || (message.data_.has_value() && !is_valid_utf8(message.data_->view()))) {
        throw std::invalid_argument("SSE fields must be valid UTF-8");
    }

    std::string frame;
    std::array<char, http_max_unsigned_decimal_size> retry_buffer{};
    std::string_view retry_digits{};
    std::size_t total_size = 1U;
    const auto add_size = [&frame, &total_size](std::size_t bytes) {
        if (bytes > frame.max_size() - total_size) {
            throw std::length_error("SSE frame exceeds string capacity");
        }
        total_size += bytes;
    };
    if (!message.event_.empty()) {
        add_size(7U);
        add_size(message.event_.size());
        add_size(1U);
    }
    if (message.id_.has_value()) {
        add_size(3U);
        if (!message.id_->empty()) {
            add_size(1U);
            add_size(message.id_->size());
        }
        add_size(1U);
    }
    if (message.retry_.has_value()) {
        retry_digits = http_format_unsigned(retry_buffer, static_cast<std::uint64_t>(message.retry_->count()));
        add_size(7U);
        add_size(retry_digits.size());
        add_size(1U);
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&add_size](std::string_view line) {
            add_size(6U);
            add_size(line.size());
            add_size(1U);
        });
    }

    frame.reserve(total_size);
    if (!message.event_.empty()) {
        frame.append("event: ");
        frame.append(message.event_.data(), message.event_.size());
        frame.push_back('\n');
    }
    if (message.id_.has_value()) {
        frame.append("id:");
        if (!message.id_->empty()) {
            frame.push_back(' ');
            frame.append(message.id_->data(), message.id_->size());
        }
        frame.push_back('\n');
    }
    if (message.retry_.has_value()) {
        frame.append("retry: ");
        frame.append(retry_digits);
        frame.push_back('\n');
    }
    if (message.data_.has_value()) {
        visit_sse_data_lines(message.data_->view(), [&frame](std::string_view line) {
            frame.append("data: ");
            frame.append(line.data(), line.size());
            frame.push_back('\n');
        });
    }
    frame.push_back('\n');
    return frame;
}

}  // namespace ruvia
