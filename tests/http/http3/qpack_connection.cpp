#include <array>
#include <string>
#include <vector>

#include "ruvia/http/Http3QpackConnection.h"

#include "test_harness.h"

namespace {
bool collect(void* context, ruvia::Http3FieldSectionFieldView field) {
    auto& fields = *static_cast<std::vector<std::pair<std::string, std::string>>*>(context);
    fields.emplace_back(field.name, field.value);
    return true;
}
}  // namespace
RUVIA_TEST(http3_qpack_dynamic_blocking_and_acknowledgment) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 1});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
    auto section = encoder.encode(0, fields);
    RUVIA_CHECK(section.has_value());
    std::vector<std::pair<std::string, std::string>> received;
    auto blocked = decoder.decode(0, *section, collect, &received);
    RUVIA_CHECK(blocked.has_value());
    RUVIA_CHECK(blocked->status == ruvia::Http3QpackDecodeStatus::kBlocked);
    RUVIA_CHECK(received.empty());
    RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 1u);
    auto instructions = encoder.pendingEncoderOutput();
    for (const char& byte : instructions) {
        RUVIA_CHECK(decoder.consumeEncoder({&byte, 1}).has_value());
    }
    RUVIA_CHECK(encoder.consumeEncoderOutput(instructions.size()));
    RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 0u);
    auto decoded = decoder.decode(0, *section, collect, &received);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK(decoded->status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
    RUVIA_CHECK_EQ(received.front().first, std::string("x-name"));
    RUVIA_CHECK_EQ(received.front().second, std::string("one"));
    RUVIA_CHECK(encoder.consumeDecoder(decoder.pendingDecoderOutput()).has_value());
    RUVIA_CHECK_EQ(encoder.knownReceivedCount(), 1u);
    auto second = encoder.encode(4, fields);
    RUVIA_CHECK(second.has_value());
    RUVIA_CHECK(encoder.pendingEncoderOutput().empty());
    RUVIA_CHECK(decoder.decode(4, *second, collect, &received).has_value());
}
RUVIA_TEST(http3_qpack_zero_blocked_allowance_uses_literals) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 0});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 0});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"private", "secret", true}, ruvia::Http3FieldSectionFieldView{"other", "data"}};
    auto section = encoder.encode(0, fields);
    RUVIA_CHECK(section.has_value());
    RUVIA_CHECK_EQ(encoder.insertCount(), 0u);
    std::vector<std::pair<std::string, std::string>> received;
    auto result = decoder.decode(0, *section, collect, &received);
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 2u);
}
RUVIA_TEST(http3_qpack_cancellation_releases_references) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    const std::array first{ruvia::Http3FieldSectionFieldView{"a", "one"}};
    auto section = encoder.encode(0, first);
    RUVIA_CHECK(section.has_value());
    RUVIA_CHECK(decoder.consumeEncoder(encoder.pendingEncoderOutput()).has_value());
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));
    RUVIA_CHECK(decoder.cancel(0).has_value());
    RUVIA_CHECK(encoder.consumeDecoder(decoder.pendingDecoderOutput()).has_value());
    const std::array second{ruvia::Http3FieldSectionFieldView{"b", "two"}};
    auto next = encoder.encode(4, second);
    RUVIA_CHECK(next.has_value());
    RUVIA_CHECK_EQ(encoder.insertCount(), 2u);
}
RUVIA_TEST(http3_qpack_decoder_rejects_invalid_instructions_and_critical_fin) {
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 0});
    const std::array bytes{char(0x21)};
    auto invalid = decoder.consumeEncoder(bytes);
    RUVIA_CHECK(!invalid.has_value());
    RUVIA_CHECK(invalid.error() == ruvia::Http3QpackConnectionError::kEncoderStreamError);
    ruvia::Http3QpackEncoder encoder({});
    auto ended = encoder.consumeDecoder({}, true);
    RUVIA_CHECK(!ended.has_value());
    RUVIA_CHECK(ended.error() == ruvia::Http3QpackConnectionError::kClosedCriticalStream);
}

RUVIA_TEST(http3_qpack_decoder_allows_required_insert_count_greater_than_highest_reference) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    const std::array fields1{ruvia::Http3FieldSectionFieldView{"x-first", "one"}};
    auto sec1 = encoder.encode(0, fields1);
    RUVIA_CHECK(sec1.has_value());
    RUVIA_CHECK(decoder.consumeEncoder(encoder.pendingEncoderOutput()).has_value());
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));

    const std::array fields2{ruvia::Http3FieldSectionFieldView{"x-second", "two"}};
    auto sec2 = encoder.encode(4, fields2);
    RUVIA_CHECK(sec2.has_value());
    RUVIA_CHECK(decoder.consumeEncoder(encoder.pendingEncoderOutput()).has_value());
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));

    RUVIA_CHECK_EQ(decoder.insertCount(), 2u);

    // Section with Required Insert Count = 2, Base = 0, referencing only Entry 0 (x-first: one).
    // highest is 1 (entry 0 + 1), which is <= required (2).
    const std::array<char, 3> sectionBytes{'\x03', '\x81', '\x10'};
    std::vector<std::pair<std::string, std::string>> received;
    auto result = decoder.decode(8, sectionBytes, collect, &received);
    RUVIA_CHECK(result.has_value());
    RUVIA_CHECK(result->status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
    if (received.size() != 1) {
        return;
    }
    RUVIA_CHECK_EQ(received[0].first, std::string("x-first"));
    RUVIA_CHECK_EQ(received[0].second, std::string("one"));
}

namespace {
struct QpackResource final : std::pmr::memory_resource {
    std::size_t liveBytes{0};
    bool fail{false};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        liveBytes += bytes;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(http3_qpack_repeated_operations_release_sections_and_preserve_retained_results) {
    QpackResource resource;
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 2}, &resource);
        ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 2}, &resource);
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
        auto retained = encoder.encode(512, fields);
        RUVIA_CHECK(retained.has_value());
        const std::vector<char> snapshot(retained->begin(), retained->end());
        RUVIA_CHECK(decoder.consumeEncoder(encoder.pendingEncoderOutput()));
        RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));
        std::vector<std::pair<std::string, std::string>> received;
        RUVIA_CHECK(decoder.decode(512, *retained, collect, &received));
        RUVIA_CHECK(encoder.consumeDecoder(decoder.pendingDecoderOutput()));
        RUVIA_CHECK(decoder.consumeDecoderOutput(decoder.pendingDecoderOutput().size()));
        const auto baseline = resource.liveBytes;
        for (std::uint64_t i = 1; i <= 64; ++i) {
            {
                auto section = encoder.encode(i * 4, fields);
                RUVIA_CHECK(section.has_value());
                received.clear();
                RUVIA_CHECK(decoder.decode(i * 4, *section, collect, &received));
                RUVIA_CHECK(encoder.consumeDecoder(decoder.pendingDecoderOutput()));
                RUVIA_CHECK(decoder.consumeDecoderOutput(decoder.pendingDecoderOutput().size()));
            }
            RUVIA_CHECK_EQ(resource.liveBytes, baseline);
            RUVIA_CHECK(std::equal(retained->begin(), retained->end(), snapshot.begin(), snapshot.end()));
        }
    }
    RUVIA_CHECK_EQ(resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_cancels_blocked_section_before_table_capacity_instruction_arrives) {
    QpackResource resource;
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
        ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
        const auto section = encoder.encode(0, fields);
        RUVIA_CHECK(section.has_value());
        const auto blocked = decoder.decode(0, *section, nullptr, nullptr);
        RUVIA_CHECK(blocked && blocked->status == ruvia::Http3QpackDecodeStatus::kBlocked);
        RUVIA_CHECK(decoder.cancel(0));
        RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 0u);
        RUVIA_CHECK(!decoder.pendingDecoderOutput().empty());
        RUVIA_CHECK(encoder.consumeDecoder(decoder.pendingDecoderOutput()));
    }
    RUVIA_CHECK_EQ(resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_allocator_failure_latches_error_and_releases_all_storage) {
    QpackResource resource;
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-long-name-to-force-allocation", "one"}};
        resource.fail = true;
        bool threw = false;
        try {
            (void)encoder.encode(0, fields);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail = false;
        RUVIA_CHECK(!encoder.encode(4, fields));
    }
    RUVIA_CHECK_EQ(resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_outstanding_budget_falls_back_to_literal_sections) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 2, .maxOutstandingSections = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
    const auto first = encoder.encode(0, fields);
    RUVIA_CHECK(first.has_value());
    const auto second = encoder.encode(4, fields);
    RUVIA_CHECK(second && (*second)[0] == 0);
    std::vector<std::pair<std::string, std::string>> received;
    const auto decoded = decoder.decode(4, *second, collect, &received);
    RUVIA_CHECK(decoded && decoded->status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
}

RUVIA_TEST(http3_qpack_rfc9204_appendix_dynamic_instruction_vectors) {
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 220, .maxBlockedStreams = 2});
    const auto hex = [](std::string_view input) {
        std::vector<char> result;
        const auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
        for (std::size_t i = 0; i < input.size(); i += 2) {
            result.push_back(static_cast<char>((nibble(input[i]) << 4) | nibble(input[i + 1])));
        }
        return result;
    };
    // Appendix B.2: capacity, static-name insertions, post-base references.
    const auto first = hex("3fbd01c00f7777772e6578616d706c652e636f6dc10c2f73616d706c652f70617468");
    for (const char& byte : first) {
        RUVIA_CHECK(decoder.consumeEncoder({&byte, 1}));
    }
    std::vector<std::pair<std::string, std::string>> received;
    const auto section = hex("03811011");
    RUVIA_CHECK(decoder.decode(4, section, collect, &received));
    RUVIA_CHECK_EQ(received.size(), 2u);
    if (received.size() != 2) {
        return;
    }
    RUVIA_CHECK_EQ(received[0].first, std::string(":authority"));
    RUVIA_CHECK_EQ(received[0].second, std::string("www.example.com"));
    RUVIA_CHECK_EQ(received[1].second, std::string("/sample/path"));
    // Appendix B.3-B.4: literal name, duplicate, blocked relative references.
    RUVIA_CHECK(decoder.consumeEncoder(hex("4a637573746f6d2d6b65790c637573746f6d2d76616c7565")));
    received.clear();
    const auto blocked = decoder.decode(8, hex("050080c181"), collect, &received);
    RUVIA_CHECK(blocked && blocked->status == ruvia::Http3QpackDecodeStatus::kBlocked);
    RUVIA_CHECK(decoder.consumeEncoder(hex("02")));
    RUVIA_CHECK(decoder.decode(8, hex("050080c181"), collect, &received));
    RUVIA_CHECK_EQ(received.size(), 3u);
    // Appendix B.5: insertion referencing a dynamic name and table eviction.
    RUVIA_CHECK(decoder.consumeEncoder(hex("810d637573746f6d2d76616c756532")));
    received.clear();
    RUVIA_CHECK(decoder.decode(12, hex("060080"), collect, &received));
    RUVIA_CHECK_EQ(received.size(), 1u);
    if (received.size() == 1) {
        RUVIA_CHECK_EQ(received[0].second, std::string("custom-value2"));
    }
}
