#include "ruvia/http/Http3QpackConnection.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "ruvia/http/Http3Qpack.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {
namespace {
using Error = Http3QpackConnectionError;
struct FailureGuard {
    std::optional<Error>& failure;
    Error error;
    int exceptions{std::uncaught_exceptions()};
    ~FailureGuard() {
        if (std::uncaught_exceptions() > exceptions) {
            failure = error;
        }
    }
};
void integer(std::pmr::vector<char>& out, std::uint8_t bits, std::uint8_t flags, std::uint64_t value) {
    std::array<char, 11> data{};
    auto size = encodeHttp3QpackInteger(data, bits, flags, value);
    if (!size) {
        throw std::logic_error("QPACK integer encoding failed");
    }
    out.insert(out.end(), data.begin(), data.begin() + *size);
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
struct Entry {
    std::pmr::string name, value;
    std::uint64_t absolute;
    std::size_t references{0};
    Entry(std::string_view n, std::string_view v, std::uint64_t index, std::pmr::memory_resource* r)
        : name(n, r),
          value(v, r),
          absolute(index) {}
    std::size_t size() const noexcept {
        return name.size() + value.size() + 32;
    }
};
struct Table {
    std::pmr::deque<Entry> entries;
    std::size_t capacity{0}, used{0};
    std::uint64_t count{0};
    std::uint64_t evictableBefore{kHttp3VarIntMax};
    explicit Table(std::pmr::memory_resource* r)
        : entries(r) {}
    const Entry* at(std::uint64_t index) const noexcept {
        if (entries.empty() || index < entries.front().absolute || index >= count) {
            return nullptr;
        }
        return &entries[static_cast<std::size_t>(index - entries.front().absolute)];
    }
    Entry* at(std::uint64_t index) noexcept {
        return const_cast<Entry*>(std::as_const(*this).at(index));
    }
    bool evictTo(std::size_t budget) {
        std::size_t removable = 0;
        for (const auto& e : entries) {
            if (used - removable <= budget) {
                break;
            }
            if (e.references || e.absolute >= evictableBefore) {
                return false;
            }
            removable += e.size();
        }
        while (used > budget && !entries.empty()) {
            used -= entries.front().size();
            entries.pop_front();
        }
        return used <= budget;
    }
    bool insert(std::string_view name, std::string_view value) {
        if (name.size() > capacity || value.size() > capacity - name.size() ||
            capacity - name.size() - value.size() < 32 || count == kHttp3VarIntMax) {
            return false;
        }
        // Copy first: a duplicate may refer to the entry evicted by this insert.
        Entry candidate(name, value, count, entries.get_allocator().resource());
        if (!evictTo(capacity - candidate.size())) {
            return false;
        }
        entries.push_back(std::move(candidate));
        used += entries.back().size();
        ++count;
        return true;
    }
};
std::optional<std::size_t> staticIndex(std::string_view name, std::optional<std::string_view> value) {
    for (std::size_t i = 0; i < 99; ++i) {
        auto e = http3QpackStaticEntry(i);
        if (e->name == name && (!value || *value == e->value)) {
            return i;
        }
    }
    return {};
}
void staticLiteral(std::pmr::vector<char>& out, Http3FieldSectionFieldView field) {
    if (auto exact = staticIndex(field.name, field.value); exact && !field.neverIndexed) {
        integer(out, 6, 0xc0, *exact);
        return;
    }
    if (auto name = staticIndex(field.name, {})) {
        integer(out, 4, field.neverIndexed ? 0x70 : 0x50, *name);
    } else {
        literal(out, 3, field.neverIndexed ? 0x30 : 0x20, field.name);
    }
    literal(out, 7, 0, field.value);
}
void validateConfig(std::size_t capacity, std::size_t blocked) {
    if (capacity > kHttp3VarIntMax || blocked > kHttp3VarIntMax) {
        throw std::invalid_argument("QPACK limits exceed QUIC varint range");
    }
}
}  // namespace

struct Http3QpackDecoder::Impl {
    Http3QpackDecoderConfig config;
    Table table;
    std::pmr::vector<char> input, output;
    std::pmr::unordered_map<std::uint64_t, std::uint64_t> blocked;
    std::optional<Error> failure;
    bool decoding{false};
    Impl(Http3QpackDecoderConfig c, std::pmr::memory_resource* r)
        : config(c),
          table(r),
          input(r),
          output(r),
          blocked(r) {}
    std::expected<bool, Error> instruction() {
        if (input.empty()) {
            return false;
        }
        auto bytes = std::span<const char>(input);
        auto first = static_cast<std::uint8_t>(bytes[0]);
        auto index = decodeHttp3QpackInteger(bytes, (first & 0x80) ? 6 : (first & 0x40) ? 5
                                                                                        : 5);
        if (!index) {
            return index.error() == Http3QpackError::kNeedMoreData ? std::expected<bool, Error>(false) : std::unexpected(Error::kEncoderStreamError);
        }
        std::size_t offset = index->encodedBytes;
        if ((first & 0xe0) == 0x20) {
            if (index->value > config.maxTableCapacity || !table.evictTo(static_cast<std::size_t>(index->value))) {
                return std::unexpected(Error::kEncoderStreamError);
            }
            table.capacity = static_cast<std::size_t>(index->value);
        } else if ((first & 0xe0) == 0) {
            if (index->value >= table.count) {
                return std::unexpected(Error::kEncoderStreamError);
            }
            auto entry = table.at(table.count - index->value - 1);
            if (!entry || !table.insert(entry->name, entry->value)) {
                return std::unexpected(Error::kEncoderStreamError);
            }
        } else {
            // Wait for both literals before decoding either one: otherwise a
            // fragmented value repeatedly allocates and decodes its complete name.
            const auto bound = std::min(config.maxTableCapacity, (std::numeric_limits<std::size_t>::max() - 64) / 4) * 4 + 64;
            if (!(first & 0x80)) {
                if (index->value > bound) {
                    return std::unexpected(Error::kEncoderStreamError);
                }
                if (index->value > bytes.size() - offset) {
                    return false;
                }
                offset += static_cast<std::size_t>(index->value);
            }
            auto valueLength = decodeHttp3QpackInteger(bytes.subspan(offset), 7);
            if (!valueLength) {
                return valueLength.error() == Http3QpackError::kNeedMoreData
                           ? std::expected<bool, Error>(false)
                           : std::unexpected(Error::kEncoderStreamError);
            }
            if (valueLength->value > bound) {
                return std::unexpected(Error::kEncoderStreamError);
            }
            if (valueLength->value > bytes.size() - offset - valueLength->encodedBytes) {
                return false;
            }
            offset = index->encodedBytes;
            std::pmr::string name(input.get_allocator().resource()), value(input.get_allocator().resource());
            if (first & 0x80) {
                if (first & 0x40) {
                    auto e = http3QpackStaticEntry(index->value);
                    if (!e) {
                        return std::unexpected(Error::kEncoderStreamError);
                    }
                    name = e->name;
                } else {
                    if (index->value >= table.count) {
                        return std::unexpected(Error::kEncoderStreamError);
                    }
                    auto e = table.at(table.count - index->value - 1);
                    if (!e) {
                        return std::unexpected(Error::kEncoderStreamError);
                    }
                    name = e->name;
                }
            } else {
                auto size = decodeHttp3QpackString(bytes, 5, name);
                if (!size) {
                    return size.error() == Http3QpackError::kNeedMoreData ? std::expected<bool, Error>(false) : std::unexpected(Error::kEncoderStreamError);
                }
                offset = *size;
            }
            auto size = decodeHttp3QpackString(bytes.subspan(offset), value);
            if (!size) {
                return size.error() == Http3QpackError::kNeedMoreData ? std::expected<bool, Error>(false) : std::unexpected(Error::kEncoderStreamError);
            }
            offset += *size;
            if (!table.insert(name, value)) {
                return std::unexpected(Error::kEncoderStreamError);
            }
        }
        input.erase(input.begin(), input.begin() + offset);
        return true;
    }
};
Http3QpackDecoder::Http3QpackDecoder(Http3QpackDecoderConfig config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      impl_(nullptr) {
    validateConfig(config.maxTableCapacity, config.maxBlockedStreams);
    std::pmr::polymorphic_allocator<Impl> a(resource_);
    impl_ = a.new_object<Impl>(config, resource_);
}
Http3QpackDecoder::~Http3QpackDecoder() {
    std::pmr::polymorphic_allocator<Impl>(resource_).delete_object(impl_);
}
std::expected<void, Error> Http3QpackDecoder::consumeEncoder(std::span<const char> bytes, bool fin) {
    auto& s = *impl_;
    if (s.decoding) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure) {
        return std::unexpected(*s.failure);
    }
    if (fin) {
        s.failure = Error::kClosedCriticalStream;
        return std::unexpected(*s.failure);
    }
    FailureGuard guard{s.failure, Error::kEncoderStreamError};
    auto before = s.table.count;
    // Feed one byte at a time so a claimed literal length cannot cause an
    // unbounded instruction buffer. Large batches contain arbitrarily many instructions.
    for (char byte : bytes) {
        if (s.input.size() >= std::min(s.config.maxTableCapacity, (std::numeric_limits<std::size_t>::max() - 64) / 4) * 4 + 64) {
            s.failure = Error::kEncoderStreamError;
            return std::unexpected(*s.failure);
        }
        s.input.push_back(byte);
        auto result = s.instruction();
        if (!result) {
            s.failure = result.error();
            return std::unexpected(*s.failure);
        }
    }
    if (s.table.count > before) {
        if (s.output.size() > s.config.maxPendingOutputBytes || s.config.maxPendingOutputBytes - s.output.size() < 11) {
            s.failure = Error::kLimit;
            return std::unexpected(*s.failure);
        }
        integer(s.output, 6, 0, s.table.count - before);
    }
    std::erase_if(s.blocked, [&](const auto& item) { return item.second <= s.table.count; });
    return {};
}
std::expected<Http3QpackDecodeResult, Error> Http3QpackDecoder::decode(std::uint64_t streamId, std::span<const char> section,
    Http3FieldSectionCallback callback, void* context) {
    auto& s = *impl_;
    if (s.decoding) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure) {
        return std::unexpected(*s.failure);
    }
    if (streamId > kHttp3VarIntMax) {
        return std::unexpected(Error::kInvalidStreamId);
    }
    auto fail = [&](Error e) -> std::expected<Http3QpackDecodeResult, Error> {s.failure=e;return std::unexpected(e); };
    if (section.size() > s.config.fields.maxEncodedBytes) {
        return fail(Error::kLimit);
    }
    FailureGuard exceptionGuard{s.failure, Error::kDecompressionFailed};
    auto encoded = decodeHttp3QpackInteger(section, 8);
    if (!encoded || encoded->encodedBytes >= section.size()) {
        return fail(Error::kDecompressionFailed);
    }
    auto delta = decodeHttp3QpackInteger(section.subspan(encoded->encodedBytes), 7);
    if (!delta) {
        return fail(Error::kDecompressionFailed);
    }
    std::uint64_t required = 0;
    if (encoded->value) {
        auto maxEntries = s.config.maxTableCapacity / 32;
        auto fullRange = 2 * maxEntries;
        if (!fullRange || encoded->value > fullRange) {
            return fail(Error::kDecompressionFailed);
        }
        auto maxValue = s.table.count + maxEntries;
        required = (maxValue / fullRange) * fullRange + encoded->value - 1;
        if (required > maxValue) {
            if (required <= fullRange) {
                return fail(Error::kDecompressionFailed);
            }
            required -= fullRange;
        }
        if (!required) {
            return fail(Error::kDecompressionFailed);
        }
    }
    bool negative = (static_cast<std::uint8_t>(section[encoded->encodedBytes]) & 0x80) != 0;
    if ((negative && delta->value >= required) || (!negative && delta->value > kHttp3VarIntMax - required)) {
        return fail(Error::kDecompressionFailed);
    }
    auto base = negative ? required - delta->value - 1 : required + delta->value;
    if (required > s.table.count) {
        if (!s.blocked.contains(streamId) && s.blocked.size() >= s.config.maxBlockedStreams) {
            return fail(Error::kDecompressionFailed);
        }
        s.blocked[streamId] = required;
        return Http3QpackDecodeResult{Http3QpackDecodeStatus::kBlocked, 0};
    }
    s.blocked.erase(streamId);
    struct Guard {
        std::optional<Error>& failure;
        int exceptions{std::uncaught_exceptions()};
        bool& value;
        Guard(bool& v, std::optional<Error>& f)
            : failure(f),
              value(v) {
            value = true;
        }
        ~Guard() {
            value = false;
            if (std::uncaught_exceptions() > exceptions) {
                failure = Error::kDecompressionFailed;
            }
        }
    } guard(s.decoding, s.failure);
    std::size_t offset = encoded->encodedBytes + delta->encodedBytes, count = 0, total = 0;
    bool callbackStopped = false;
    std::uint64_t highest = 0;
    std::pmr::string name(resource_), value(resource_);
    auto dynamic = [&](std::uint64_t index, bool post) -> const Entry* {
        if ((post && index > kHttp3VarIntMax - base) || (!post && index >= base)) {
            return nullptr;
        }
        auto absolute = post ? base + index : base - index - 1;
        if (absolute >= required) {
            return nullptr;
        }
        auto e = s.table.at(absolute);
        if (e) {
            highest = std::max(highest, absolute + 1);
        }
        return e;
    };
    while (offset < section.size()) {
        auto bytes = section.subspan(offset);
        auto first = static_cast<std::uint8_t>(bytes[0]);
        Http3FieldSectionFieldView field{};
        bool indexed = (first & 0x80) != 0, nameReference = (first & 0xc0) == 0x40,
             literalName = (first & 0xe0) == 0x20, postIndexed = (first & 0xf0) == 0x10;
        if (indexed || nameReference || postIndexed || !literalName) {
            auto index = decodeHttp3QpackInteger(bytes, indexed ? 6 : nameReference ? 4
                                                                  : postIndexed     ? 4
                                                                                    : 3);
            if (!index) {
                return fail(Error::kDecompressionFailed);
            }
            offset += index->encodedBytes;
            bool isStatic = indexed ? (first & 0x40) != 0 : nameReference && (first & 0x10) != 0;
            if (isStatic) {
                auto e = http3QpackStaticEntry(index->value);
                if (!e) {
                    return fail(Error::kDecompressionFailed);
                }
                field = {e->name, e->value, false};
            } else {
                auto e = dynamic(index->value, postIndexed || (!indexed && !nameReference));
                if (!e) {
                    return fail(Error::kDecompressionFailed);
                }
                field = {e->name, e->value, false};
            }
            if (!indexed && !postIndexed) {
                auto consumed = decodeHttp3QpackString(section.subspan(offset), value);
                if (!consumed) {
                    return fail(Error::kDecompressionFailed);
                }
                offset += *consumed;
                field.value = value;
                field.neverIndexed = (first & (nameReference ? 0x20 : 0x08)) != 0;
            }
        } else {
            auto n = decodeHttp3QpackString(bytes, 3, name);
            if (!n) {
                return fail(Error::kDecompressionFailed);
            }
            offset += *n;
            auto v = decodeHttp3QpackString(section.subspan(offset), value);
            if (!v) {
                return fail(Error::kDecompressionFailed);
            }
            offset += *v;
            field = {name, value, (first & 0x10) != 0};
        }
        if (count >= s.config.fields.maxFields || total > s.config.fields.maxDecodedBytes ||
            s.config.fields.maxDecodedBytes - total < 32 || field.name.size() > s.config.fields.maxDecodedBytes - total - 32 ||
            field.value.size() > s.config.fields.maxDecodedBytes - total - 32 - field.name.size()) {
            return fail(Error::kLimit);
        }
        total += 32 + field.name.size() + field.value.size();
        ++count;
        if (callback && !callbackStopped && !callback(context, field)) {
            callbackStopped = true;
        }
    }
    if (highest > required) {
        return fail(Error::kDecompressionFailed);
    }
    if (required) {
        if (s.output.size() > s.config.maxPendingOutputBytes || s.config.maxPendingOutputBytes - s.output.size() < 11) {
            return fail(Error::kLimit);
        }
        integer(s.output, 7, 0x80, streamId);
    }
    return Http3QpackDecodeResult{callbackStopped ? Http3QpackDecodeStatus::kCallbackStopped : Http3QpackDecodeStatus::kDecoded, count};
}
std::expected<void, Error> Http3QpackDecoder::cancel(std::uint64_t streamId) {
    auto& s = *impl_;
    if (s.decoding) {
        throw std::logic_error("reentrant QPACK operation");
    }
    if (s.failure) {
        return std::unexpected(*s.failure);
    }
    if (streamId > kHttp3VarIntMax) {
        return std::unexpected(Error::kInvalidStreamId);
    }
    s.blocked.erase(streamId);
    if (s.config.maxTableCapacity == 0) {
        return {};
    }
    if (s.output.size() > s.config.maxPendingOutputBytes || s.config.maxPendingOutputBytes - s.output.size() < 11) {
        s.failure = Error::kLimit;
        return std::unexpected(*s.failure);
    }
    FailureGuard guard{s.failure, Error::kDecompressionFailed};
    integer(s.output, 6, 0x40, streamId);
    return {};
}
std::span<const char> Http3QpackDecoder::pendingDecoderOutput() const& noexcept {
    return impl_->output;
}
bool Http3QpackDecoder::consumeDecoderOutput(std::size_t bytes) noexcept {
    return !impl_->decoding && consume(impl_->output, bytes);
}
std::uint64_t Http3QpackDecoder::insertCount() const noexcept {
    return impl_->table.count;
}
std::size_t Http3QpackDecoder::blockedStreamCount() const noexcept {
    return impl_->blocked.size();
}

struct Http3QpackEncoder::Impl {
    struct Section {
        std::uint64_t required;
        std::pmr::vector<std::uint64_t> references;
        Section(std::uint64_t ric, std::pmr::vector<std::uint64_t> refs)
            : required(ric),
              references(std::move(refs)) {}
    };
    Http3QpackEncoderConfig config;
    Table table;
    std::pmr::vector<char> input, output;
    std::pmr::unordered_map<std::uint64_t, std::pmr::deque<Section>> sections;
    std::uint64_t received{0};
    std::size_t outstandingSections{0};
    std::optional<Error> failure;
    Impl(Http3QpackEncoderConfig c, std::pmr::memory_resource* r)
        : config(c),
          table(r),
          input(r),
          output(r),
          sections(r) {
        table.capacity = c.tableCapacity.value_or(c.maxTableCapacity);
        table.evictableBefore = 0;
        if (table.capacity) {
            integer(output, 5, 0x20, table.capacity);
        }
    }
    void release(const Section& section) {
        for (auto absolute : section.references) {
            auto e = table.at(absolute);
            if (e && e->references) {
                --e->references;
            }
        }
    }
    std::size_t blockedCount() const {
        std::size_t n = 0;
        for (const auto& [id, list] : sections) {
            (void)id;
            if (std::any_of(list.begin(), list.end(), [&](const auto& section) { return section.required > received; })) {
                ++n;
            }
        }
        return n;
    }
};
Http3QpackEncoder::Http3QpackEncoder(Http3QpackEncoderConfig config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      impl_(nullptr) {
    validateConfig(config.maxTableCapacity, config.maxBlockedStreams);
    if ((config.tableCapacity && *config.tableCapacity > config.maxTableCapacity) ||
        config.maxPendingOutputBytes < 11) {
        throw std::invalid_argument("invalid QPACK encoder limits");
    }
    impl_ = std::pmr::polymorphic_allocator<Impl>(resource_).new_object<Impl>(config, resource_);
}
Http3QpackEncoder::~Http3QpackEncoder() {
    std::pmr::polymorphic_allocator<Impl>(resource_).delete_object(impl_);
}
std::expected<std::pmr::vector<char>, Error> Http3QpackEncoder::encode(std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits) {
    auto& s = *impl_;
    if (s.failure) {
        return std::unexpected(*s.failure);
    }
    if (streamId > kHttp3VarIntMax) {
        return std::unexpected(Error::kInvalidStreamId);
    }
    limits.maxFields = std::min(limits.maxFields, s.config.fields.maxFields);
    limits.maxDecodedBytes = std::min(limits.maxDecodedBytes, s.config.fields.maxDecodedBytes);
    limits.maxEncodedBytes = std::min(limits.maxEncodedBytes, s.config.fields.maxEncodedBytes);
    std::size_t total = 0;
    if (fields.size() > limits.maxFields) {
        return std::unexpected(Error::kLimit);
    }
    for (const auto& f : fields) {
        if (total > limits.maxDecodedBytes || limits.maxDecodedBytes - total < 32 ||
            f.name.size() > limits.maxDecodedBytes - total - 32 ||
            f.value.size() > limits.maxDecodedBytes - total - 32 - f.name.size()) {
            return std::unexpected(Error::kLimit);
        }
        total += 32 + f.name.size() + f.value.size();
    }
    FailureGuard exceptionGuard{s.failure, Error::kDecoderStreamError};
    auto existing = s.sections.find(streamId);
    bool alreadyBlocked = existing != s.sections.end() && std::any_of(existing->second.begin(), existing->second.end(), [&](const auto& section) { return section.required > s.received; });
    const bool mayReference = s.outstandingSections < s.config.maxOutstandingSections;
    bool mayBlock = mayReference && (alreadyBlocked || s.blockedCount() < s.config.maxBlockedStreams);
    std::pmr::vector<char> body(resource_);
    std::pmr::vector<std::uint64_t> references(resource_);
    std::uint64_t required = 0;
    for (const auto& f : fields) {
        if (!mayReference || f.neverIndexed || staticIndex(f.name, f.value)) {
            staticLiteral(body, f);
            continue;
        }
        Entry* selected = nullptr;
        for (auto& e : s.table.entries) {
            if (e.name == f.name && e.value == f.value && (mayBlock || e.absolute < s.received)) {
                selected = &e;
                break;
            }
        }
        const auto instructionLimit = s.config.maxPendingOutputBytes;
        const bool canInsert = s.output.size() <= instructionLimit && instructionLimit - s.output.size() >= 22 &&
                               f.name.size() <= instructionLimit - s.output.size() - 22 &&
                               f.value.size() <= instructionLimit - s.output.size() - 22 - f.name.size();
        if (!selected && mayBlock && canInsert && s.table.insert(f.name, f.value)) {
            literal(s.output, 5, 0x40, f.name);
            literal(s.output, 7, 0, f.value);
            selected = &s.table.entries.back();
        }
        if (!selected) {
            staticLiteral(body, f);
            continue;
        }
        // Base is zero, so all dynamic references use post-base indexing.
        integer(body, 4, 0x10, selected->absolute);
        required = std::max(required, selected->absolute + 1);
        references.push_back(selected->absolute);
        ++selected->references;
    }
    std::pmr::vector<char> output(resource_);
    integer(output, 8, 0, required ? required % (2 * (s.config.maxTableCapacity / 32)) + 1 : 0);
    integer(output, 7, required ? 0x80 : 0, required ? required - 1 : 0);
    output.insert(output.end(), body.begin(), body.end());
    if (output.size() > limits.maxEncodedBytes) {
        for (auto index : references) {
            --s.table.at(index)->references;
        }
        return std::unexpected(Error::kLimit);
    }
    if (required) {
        s.sections[streamId].emplace_back(required, std::move(references));
        ++s.outstandingSections;
    }
    return output;
}
std::expected<void, Error> Http3QpackEncoder::consumeDecoder(std::span<const char> bytes, bool fin) {
    auto& s = *impl_;
    if (s.failure) {
        return std::unexpected(*s.failure);
    }
    auto fail = [&](Error e) -> std::expected<void, Error> {s.failure=e;return std::unexpected(e); };
    if (fin) {
        return fail(Error::kClosedCriticalStream);
    }
    FailureGuard exceptionGuard{s.failure, Error::kDecoderStreamError};
    for (char byte : bytes) {
        if (s.input.size() >= 11) {
            return fail(Error::kDecoderStreamError);
        }
        s.input.push_back(byte);
        auto first = static_cast<std::uint8_t>(s.input[0]);
        auto decoded = decodeHttp3QpackInteger(s.input, (first & 0x80) ? 7 : 6);
        if (!decoded) {
            if (decoded.error() == Http3QpackError::kNeedMoreData) {
                continue;
            }
            return fail(Error::kDecoderStreamError);
        }
        auto value = decoded->value;
        if (first & 0x80) {
            auto found = s.sections.find(value);
            if (found == s.sections.end() || found->second.empty()) {
                return fail(Error::kDecoderStreamError);
            }
            auto& section = found->second.front();
            s.received = std::max(s.received, section.required);
            s.release(section);
            found->second.pop_front();
            --s.outstandingSections;
            if (found->second.empty()) {
                s.sections.erase(found);
            }
        } else if (first & 0x40) {
            auto found = s.sections.find(value);
            if (found != s.sections.end()) {
                s.outstandingSections -= found->second.size();
                for (auto& section : found->second) {
                    s.release(section);
                }
                s.sections.erase(found);
            }
        } else {
            if (!value || value > s.table.count - s.received) {
                return fail(Error::kDecoderStreamError);
            }
            s.received += value;
        }
        s.table.evictableBefore = s.received;
        s.input.clear();
    }
    return {};
}
std::span<const char> Http3QpackEncoder::pendingEncoderOutput() const& noexcept {
    return impl_->output;
}
bool Http3QpackEncoder::consumeEncoderOutput(std::size_t bytes) noexcept {
    return consume(impl_->output, bytes);
}
std::uint64_t Http3QpackEncoder::insertCount() const noexcept {
    return impl_->table.count;
}
std::uint64_t Http3QpackEncoder::knownReceivedCount() const noexcept {
    return impl_->received;
}
}  // namespace ruvia
