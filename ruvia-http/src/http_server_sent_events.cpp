#include "http_server_sent_events.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "decimal_text.h"
#include "http_io_buffer.h"
#include "utf8.h"

namespace ruvia {
namespace {

// Validate UTF-8 while finding line boundaries, then copy each validated line
// as a block. In an event stream CRLF counts as a single line ending.
void append_sse_data_lines(std::string_view data, std::string& frame) {
    std::size_t begin = 0;
    for (std::size_t index = 0; index < data.size();) {
        const auto byte = static_cast<unsigned char>(data[index]);
        if (byte == '\r' || byte == '\n') {
            frame.append("data: ");
            frame.append(data.substr(begin, index - begin));
            frame.push_back('\n');
            ++index;
            if (byte == '\r' && index < data.size() && data[index] == '\n') {
                ++index;
            }
            begin = index;
        } else if (byte < 0x80) {
            ++index;
        } else {
            const auto sequence = scan_utf8_sequence(data, index);
            if (!sequence.well_formed_) {
                throw std::invalid_argument("SSE fields must be valid UTF-8");
            }
            index += sequence.length_;
        }
    }
    frame.append("data: ");
    frame.append(data.substr(begin));
    frame.push_back('\n');
}

constexpr std::string_view sse_utf8_bom = "\xEF\xBB\xBF";
constexpr std::string_view sse_replacement_character = "\xEF\xBF\xBD";
constexpr std::string_view sse_default_event_type = "message";

// The event stream is decoded with WHATWG UTF-8 decode, which replaces each
// maximal subpart of an ill-formed sequence with one U+FFFD. The decoder never
// consumes an ASCII byte, so repairing each field value after the line split
// matches decoding the stream first.
[[nodiscard]] bool replace_ill_formed_utf8(std::string& value, std::size_t max_bytes) {
    const std::string_view bytes(value);
    std::string repaired;
    std::size_t copy_start = 0;
    for (std::size_t index = 0; index < bytes.size();) {
        if (static_cast<unsigned char>(bytes[index]) < 0x80) {
            ++index;
            continue;
        }
        const auto sequence = scan_utf8_sequence(bytes, index);
        if (!sequence.well_formed_) {
            const auto valid_bytes = index - copy_start;
            const auto available = max_bytes - repaired.size();
            if (valid_bytes > available || sse_replacement_character.size() > available - valid_bytes) {
                return false;
            }
            if (copy_start == 0) {
                repaired.reserve(bytes.size());
            }
            repaired.append(bytes.substr(copy_start, valid_bytes));
            repaired.append(sse_replacement_character);
            copy_start = index + sequence.length_;
        }
        index += sequence.length_;
    }
    if (copy_start != 0) {
        if (bytes.size() - copy_start > max_bytes - repaired.size()) {
            return false;
        }
        repaired.append(bytes.substr(copy_start));
        value.swap(repaired);
    }
    return true;
}

}  // namespace

sse_parser::sse_parser(sse_parser_options options) :
    event_limit_(options.event_limit_) {}

sse_parse_result sse_parser::parse(std::string_view input) & {
    if (state_ != decoder_state::active) {
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
            return sse_parse_result(position, decode_need_input{});
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
                return sse_parse_result(input.size(), decode_need_input{});
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
                if (!dispatch()) {
                    fail();
                    return sse_parse_result(position, sse_parse_failure(sse_parse_error::event_limit_exceeded));
                }
                if (event_published_) {
                    return sse_parse_result(position, sse_event(event_type_.empty() ? sse_default_event_type : std::string_view(event_type_), data_, last_event_id_));
                }
                continue;
            }
            // A line without a colon names a field with an empty value.
            if (!end_line()) {
                fail();
                return sse_parse_result(position, sse_parse_failure(sse_parse_error::event_limit_exceeded));
            }
            continue;
        }
        const auto stop = rest.find_first_of("\r\n");
        const auto bytes = rest.substr(0, stop);
        if (phase_ == line_phase::value && !append_value(bytes)) {
            fail();
            return sse_parse_result(position, sse_parse_failure(sse_parse_error::event_limit_exceeded));
        }
        if (stop == std::string_view::npos) {
            return sse_parse_result(input.size(), decode_need_input{});
        }
        position += stop + 1;
        skip_line_feed_ = rest[stop] == '\r';
        if (phase_ == line_phase::comment) {
            reset_line();
        } else if (!end_line()) {
            fail();
            return sse_parse_result(position, sse_parse_failure(sse_parse_error::event_limit_exceeded));
        }
    }
    return sse_parse_result(position, decode_need_input{});
}

void sse_parser::finish() noexcept {
    if (state_ != decoder_state::active) {
        return;
    }
    release_event_buffers();
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(last_event_id_buffer_);
    event_id_pending_ = false;
    state_ = decoder_state::finished;
}

void sse_parser::reset() noexcept {
    event_published_ = false;
    event_id_pending_ = false;
    reset_line();
    http_clear_string_retaining_small(data_);
    http_clear_string_retaining_small(event_type_);
    http_clear_string_retaining_small(last_event_id_buffer_);
    http_clear_string_retaining_small(last_event_id_);
    reconnection_time_.reset();
    skip_line_feed_ = false;
    bom_matched_ = 0;
    bom_settled_ = false;
    state_ = decoder_state::active;
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
            retry_digits_ = true;
            retry_value_ = accumulate_decimal_digit(retry_value_, static_cast<unsigned>(byte - '0'), max_retry);
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
        case field_kind::id: {
            const bool is_id = field_ == field_kind::id;
            // An id containing NUL is ignored.
            if (is_id && value_.find('\0') != std::string::npos) {
                break;
            }
            if (!replace_ill_formed_utf8(value_, event_limit_.read_ceiling() - data_.size() - event_type_.size())) {
                return false;
            }
            (is_id ? last_event_id_buffer_ : event_type_).swap(value_);
            event_id_pending_ = event_id_pending_ || is_id;
            break;
        }
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
    if (!replace_ill_formed_utf8(data_, event_limit_.read_ceiling() - event_type_.size())) {
        return false;
    }
    if (std::exchange(event_id_pending_, false)) {
        last_event_id_.swap(last_event_id_buffer_);
        http_clear_string_retaining_small(last_event_id_buffer_);
    }
    if (data_.empty()) {
        http_clear_string_retaining_small(event_type_);
        return true;
    }
    data_.pop_back();
    event_published_ = true;
    return true;
}

void sse_parser::reset_line() noexcept {
    phase_ = line_phase::name;
    field_ = field_kind::ignored;
    name_size_ = 0;
    name_overflow_ = false;
    http_clear_string_retaining_small(value_);
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
    http_clear_string_retaining_small(last_event_id_buffer_);
    event_id_pending_ = false;
    state_ = decoder_state::failed;
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
    if (!is_valid_utf8(message.event_.view()) || (message.id_.has_value() && !is_valid_utf8(message.id_->view()))) {
        throw std::invalid_argument("SSE fields must be valid UTF-8");
    }

    std::string frame;
    std::array<char, max_decimal_size> retry_buffer{};
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
        retry_digits = format_decimal(retry_buffer, static_cast<std::uint64_t>(message.retry_->count()));
        add_size(7U);
        add_size(retry_digits.size());
        add_size(1U);
    }
    if (message.data_.has_value()) {
        // Each extra data line adds more framing bytes than its delimiter used.
        add_size(message.data_->size());
        add_size(7U);
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
        append_sse_data_lines(message.data_->view(), frame);
    }
    frame.push_back('\n');
    return frame;
}

}  // namespace ruvia
