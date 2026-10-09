#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ruvia/http/Http3Qpack.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3RequestWriter.h"

#include "http3/Http3FieldSectionEncoder.h"
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
    RUVIA_CHECK((section.index() == 0));
    std::vector<std::pair<std::string, std::string>> received;
    auto blocked = decoder.decode(0, std::get<0>(section), collect, &received);
    RUVIA_CHECK((blocked.index() == 0));
    RUVIA_CHECK(std::get<0>(blocked).status == ruvia::Http3QpackDecodeStatus::kBlocked);
    RUVIA_CHECK(received.empty());
    RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 1u);
    auto instructions = encoder.pendingEncoderOutput();
    for (const char& byte : instructions) {
        RUVIA_CHECK((decoder.consumeEncoder({&byte, 1}).index() == 0));
    }
    RUVIA_CHECK(encoder.consumeEncoderOutput(instructions.size()));
    RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 0u);
    auto decoded = decoder.decode(0, std::get<0>(section), collect, &received);
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK(std::get<0>(decoded).status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
    RUVIA_CHECK_EQ(received.front().first, std::string("x-name"));
    RUVIA_CHECK_EQ(received.front().second, std::string("one"));
    RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput()).index() == 0));
    RUVIA_CHECK_EQ(encoder.knownReceivedCount(), 1u);
    auto second = encoder.encode(4, fields);
    RUVIA_CHECK((second.index() == 0));
    RUVIA_CHECK(encoder.pendingEncoderOutput().empty());
    RUVIA_CHECK((decoder.decode(4, std::get<0>(second), collect, &received).index() == 0));
}
RUVIA_TEST(http3_qpack_zero_blocked_allowance_uses_literals) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 0});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 0});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"private", "secret", true}, ruvia::Http3FieldSectionFieldView{"other", "data"}};
    auto section = encoder.encode(0, fields);
    RUVIA_CHECK((section.index() == 0));
    RUVIA_CHECK_EQ(encoder.insertCount(), 0u);
    std::vector<std::pair<std::string, std::string>> received;
    auto result = decoder.decode(0, std::get<0>(section), collect, &received);
    RUVIA_CHECK((result.index() == 0));
    RUVIA_CHECK(std::get<0>(result).status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 2u);
}
RUVIA_TEST(http3_qpack_cancellation_releases_references) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    const std::array first{ruvia::Http3FieldSectionFieldView{"a", "one"}};
    auto section = encoder.encode(0, first);
    RUVIA_CHECK((section.index() == 0));
    RUVIA_CHECK((decoder.consumeEncoder(encoder.pendingEncoderOutput()).index() == 0));
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));
    RUVIA_CHECK((decoder.cancel(0).index() == 0));
    RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput()).index() == 0));
    const std::array second{ruvia::Http3FieldSectionFieldView{"b", "two"}};
    auto next = encoder.encode(4, second);
    RUVIA_CHECK((next.index() == 0));
    RUVIA_CHECK_EQ(encoder.insertCount(), 2u);
}
RUVIA_TEST(http3_qpack_decoder_rejects_invalid_instructions_and_critical_fin) {
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 0});
    const std::array bytes{char(0x21)};
    auto invalid = decoder.consumeEncoder(bytes);
    RUVIA_CHECK(!(invalid.index() == 0));
    RUVIA_CHECK(std::get<1>(invalid) == ruvia::Http3QpackConnectionError::kEncoderStreamError);
    ruvia::Http3QpackEncoder encoder({});
    auto ended = encoder.consumeDecoder({}, true);
    RUVIA_CHECK(!(ended.index() == 0));
    RUVIA_CHECK(std::get<1>(ended) == ruvia::Http3QpackConnectionError::kClosedCriticalStream);
}

RUVIA_TEST(http3_qpack_decoder_allows_required_insert_count_greater_than_highest_reference) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    const std::array fields1{ruvia::Http3FieldSectionFieldView{"x-first", "one"}};
    auto sec1 = encoder.encode(0, fields1);
    RUVIA_CHECK((sec1.index() == 0));
    RUVIA_CHECK((decoder.consumeEncoder(encoder.pendingEncoderOutput()).index() == 0));
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));

    const std::array fields2{ruvia::Http3FieldSectionFieldView{"x-second", "two"}};
    auto sec2 = encoder.encode(4, fields2);
    RUVIA_CHECK((sec2.index() == 0));
    RUVIA_CHECK((decoder.consumeEncoder(encoder.pendingEncoderOutput()).index() == 0));
    RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));

    RUVIA_CHECK_EQ(decoder.insertCount(), 2u);

    // Section with Required Insert Count = 2, Base = 0, referencing only Entry 0 (x-first: one).
    // highest is 1 (entry 0 + 1), which is <= required (2).
    const std::array<char, 3> sectionBytes{'\x03', '\x81', '\x10'};
    std::vector<std::pair<std::string, std::string>> received;
    auto result = decoder.decode(8, sectionBytes, collect, &received);
    RUVIA_CHECK((result.index() == 0));
    RUVIA_CHECK(std::get<0>(result).status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
    if (received.size() != 1) {
        return;
    }
    RUVIA_CHECK_EQ(received[0].first, std::string("x-first"));
    RUVIA_CHECK_EQ(received[0].second, std::string("one"));
}

namespace {
struct QpackResource final : std::pmr::memory_resource {
    explicit QpackResource(const void* equality_group = nullptr)
        : equality_group_(equality_group ? equality_group : this) {}
    const void* equality_group_;
    std::size_t liveBytes{0};
    std::size_t allocationCount{0};
    std::optional<std::size_t> failAtAllocation{};
    bool fail{false};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Keep noexcept debug iterator bookkeeping available while failing
        // the encoded field data allocation.
        const auto allocation = allocationCount++;
        if ((fail && bytes >= 32) || (failAtAllocation && allocation == *failAtAllocation)) {
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
        const auto* resource = dynamic_cast<const QpackResource*>(&other);
        return resource && equality_group_ == resource->equality_group_;
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
        RUVIA_CHECK((retained.index() == 0));
        const std::vector<char> snapshot(std::get<0>(retained).begin(), std::get<0>(retained).end());
        RUVIA_CHECK((decoder.consumeEncoder(encoder.pendingEncoderOutput())).index() == 0);
        RUVIA_CHECK(encoder.consumeEncoderOutput(encoder.pendingEncoderOutput().size()));
        std::vector<std::pair<std::string, std::string>> received;
        RUVIA_CHECK((decoder.decode(512, std::get<0>(retained), collect, &received)).index() == 0);
        RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput())).index() == 0);
        RUVIA_CHECK(decoder.consumeDecoderOutput(decoder.pendingDecoderOutput().size()));
        const auto baseline = resource.liveBytes;
        for (std::uint64_t i = 1; i <= 64; ++i) {
            {
                auto section = encoder.encode(i * 4, fields);
                RUVIA_CHECK((section.index() == 0));
                received.clear();
                RUVIA_CHECK((decoder.decode(i * 4, std::get<0>(section), collect, &received)).index() == 0);
                RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput())).index() == 0);
                RUVIA_CHECK(decoder.consumeDecoderOutput(decoder.pendingDecoderOutput().size()));
            }
            RUVIA_CHECK_EQ(resource.liveBytes, baseline);
            RUVIA_CHECK(std::equal(std::get<0>(retained).begin(), std::get<0>(retained).end(), snapshot.begin(), snapshot.end()));
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
        RUVIA_CHECK((section.index() == 0));
        const auto blocked = decoder.decode(0, std::get<0>(section), nullptr, nullptr);
        RUVIA_CHECK((blocked.index() == 0) && std::get<0>(blocked).status == ruvia::Http3QpackDecodeStatus::kBlocked);
        RUVIA_CHECK((decoder.cancel(0)).index() == 0);
        RUVIA_CHECK_EQ(decoder.blockedStreamCount(), 0u);
        RUVIA_CHECK(!decoder.pendingDecoderOutput().empty());
        RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput())).index() == 0);
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
        RUVIA_CHECK((encoder.encode(4, fields).index() != 0));
    }
    RUVIA_CHECK_EQ(resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_outstanding_budget_falls_back_to_literal_sections) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 2, .maxOutstandingSections = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 2});
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
    const auto first = encoder.encode(0, fields);
    RUVIA_CHECK((first.index() == 0));
    const auto second = encoder.encode(4, fields);
    RUVIA_CHECK((second.index() == 0) && (std::get<0>(second))[0] == 0);
    std::vector<std::pair<std::string, std::string>> received;
    const auto decoded = decoder.decode(4, std::get<0>(second), collect, &received);
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK_EQ(received.size(), 1u);
}

RUVIA_TEST(http3_qpack_encoded_limit_preserves_insertions_and_instruction_sync) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 64, .maxBlockedStreams = 1});
    const std::array first{ruvia::Http3FieldSectionFieldView{"x-name", "one"}};
    auto limited = encoder.encode(0, first, {.maxEncodedBytes = 2});
    RUVIA_CHECK((limited.index() != 0));
    RUVIA_CHECK(std::get<1>(limited) == ruvia::Http3QpackConnectionError::kLimit);
    RUVIA_CHECK_EQ(encoder.insertCount(), 1u);
    RUVIA_CHECK(!encoder.pendingEncoderOutput().empty());

    auto instructions = encoder.pendingEncoderOutput();
    RUVIA_CHECK((decoder.consumeEncoder(instructions)).index() == 0);
    RUVIA_CHECK(encoder.consumeEncoderOutput(instructions.size()));
    RUVIA_CHECK_EQ(decoder.insertCount(), 1u);

    const auto usable = encoder.encode(0, first);
    RUVIA_CHECK((usable).index() == 0);
    std::vector<std::pair<std::string, std::string>> received;
    const auto decoded = decoder.decode(0, std::get<0>(usable), collect, &received);
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).status == ruvia::Http3QpackDecodeStatus::kDecoded);
    RUVIA_CHECK((encoder.consumeDecoder(decoder.pendingDecoderOutput())).index() == 0);
    RUVIA_CHECK(decoder.consumeDecoderOutput(decoder.pendingDecoderOutput().size()));

    const std::array replacement{ruvia::Http3FieldSectionFieldView{"x-other", "two"}};
    const auto replaced = encoder.encode(4, replacement);
    RUVIA_CHECK((replaced).index() == 0);
    RUVIA_CHECK_EQ(encoder.insertCount(), 2u);
    RUVIA_CHECK(!encoder.pendingEncoderOutput().empty());
    instructions = encoder.pendingEncoderOutput();
    RUVIA_CHECK((decoder.consumeEncoder(instructions)).index() == 0);
    RUVIA_CHECK_EQ(decoder.insertCount(), 2u);
}
RUVIA_TEST(http3_qpack_late_allocator_failure_latches_terminal_error_and_releases_storage) {
    bool found_late_failure = false;
    for (std::size_t offset = 0; offset < 32 && !found_late_failure; ++offset) {
        QpackResource resource;
        {
            ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
            const std::array fields{ruvia::Http3FieldSectionFieldView{"x-long-name-to-force-late-allocation", "one"}};
            resource.failAtAllocation = resource.allocationCount + offset;
            bool threw = false;
            try {
                (void)encoder.encode(0, fields);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw && encoder.insertCount() != 0 && !encoder.pendingEncoderOutput().empty()) {
                found_late_failure = true;
                resource.failAtAllocation.reset();
                const auto encode_error = encoder.encode(4, fields);
                RUVIA_CHECK((encode_error.index() != 0));
                RUVIA_CHECK(std::get<1>(encode_error) == ruvia::Http3QpackConnectionError::kDecoderStreamError);
                const std::array<char, 1> acknowledgment{static_cast<char>(0x80)};
                const auto decoder_error = encoder.consumeDecoder(acknowledgment);
                RUVIA_CHECK((decoder_error.index() != 0));
                RUVIA_CHECK(std::get<1>(decoder_error) == ruvia::Http3QpackConnectionError::kDecoderStreamError);
            }
        }
        RUVIA_CHECK_EQ(resource.liveBytes, 0u);
    }
    RUVIA_CHECK(found_late_failure);
}
RUVIA_TEST(http3_qpack_construction_propagates_allocation_failure_and_releases_storage) {
    for (const bool encode : {false, true}) {
        bool constructed = false;
        for (std::size_t offset = 0; offset < 32 && !constructed; ++offset) {
            QpackResource resource;
            resource.failAtAllocation = resource.allocationCount + offset;
            try {
                if (encode) {
                    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
                } else {
                    ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
                }
                constructed = true;
            } catch (const std::bad_alloc&) {
            }
            RUVIA_CHECK_EQ(resource.liveBytes, 0u);
        }
        RUVIA_CHECK(constructed);
    }
}

RUVIA_TEST(http3_qpack_decoder_allocation_failure_latches_error_and_releases_storage) {
    bool decoded = false;
    for (std::size_t offset = 0; offset < 32 && !decoded; ++offset) {
        QpackResource resource;
        {
            ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &resource);
            const std::array<char, 3> section{0, 0, static_cast<char>(0xd1)};
            resource.failAtAllocation = resource.allocationCount + offset;
            try {
                std::vector<std::pair<std::string, std::string>> received;
                const auto result = decoder.decode(0, section, collect, &received);
                RUVIA_CHECK((result.index() == 0) && std::get<0>(result).status == ruvia::Http3QpackDecodeStatus::kDecoded);
                RUVIA_CHECK_EQ(received.size(), std::size_t{1});
                if (!received.empty()) {
                    RUVIA_CHECK_EQ(received.front().first, std::string(":method"));
                    RUVIA_CHECK_EQ(received.front().second, std::string("GET"));
                }
                decoded = true;
            } catch (const std::bad_alloc&) {
                resource.failAtAllocation.reset();
                const auto result = decoder.decode(4, section, nullptr, nullptr);
                RUVIA_CHECK((result.index() != 0));
                if ((result.index() != 0)) {
                    RUVIA_CHECK(std::get<1>(result) == ruvia::Http3QpackConnectionError::kDecompressionFailed);
                }
                const auto encoder_result = decoder.consumeEncoder({});
                RUVIA_CHECK((encoder_result.index() != 0));
                if ((encoder_result.index() != 0)) {
                    RUVIA_CHECK(std::get<1>(encoder_result) == ruvia::Http3QpackConnectionError::kDecompressionFailed);
                }
            }
        }
        RUVIA_CHECK_EQ(resource.liveBytes, 0u);
    }
    RUVIA_CHECK(decoded);
}

RUVIA_TEST(http3_qpack_field_section_prefixes_grow_and_wrap_across_acknowledged_insertions) {
    QpackResource result_resource;
    bool saw_wide_insert_count = false;
    bool saw_wide_base = false;
    bool saw_wrapped_insert_count = false;
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 8192, .maxBlockedStreams = 1});
        ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 8192, .maxBlockedStreams = 1});
        for (std::uint64_t sequence = 0; sequence < 600; ++sequence) {
            const auto value = std::string("value-") + std::to_string(sequence);
            const std::array fields{ruvia::Http3FieldSectionFieldView{"x-sequence", value}};
            const auto stream_id = sequence * 4;
            const auto section = encoder.encode(stream_id, fields, {}, &result_resource);
            RUVIA_CHECK((section.index() == 0));
            if ((section.index() != 0)) {
                return;
            }
            RUVIA_CHECK(std::get<0>(section).get_allocator().resource() == &result_resource);
            const auto insert_count = ruvia::decodeHttp3QpackInteger(std::get<0>(section), 8);
            RUVIA_CHECK((insert_count.index() == 0));
            if ((insert_count.index() != 0)) {
                return;
            }
            const auto base = ruvia::decodeHttp3QpackInteger(std::span(std::get<0>(section)).subspan(std::get<0>(insert_count).encodedBytes), 7);
            RUVIA_CHECK((base.index() == 0));
            if ((base.index() != 0)) {
                return;
            }
            saw_wide_insert_count = saw_wide_insert_count || std::get<0>(insert_count).encodedBytes > 1;
            saw_wide_base = saw_wide_base || std::get<0>(base).encodedBytes > 1;
            saw_wrapped_insert_count = saw_wrapped_insert_count || (sequence > 0 && std::get<0>(insert_count).value == 1);

            const auto instructions = encoder.pendingEncoderOutput();
            RUVIA_CHECK(decoder.consumeEncoder(instructions).index() == 0);
            RUVIA_CHECK(encoder.consumeEncoderOutput(instructions.size()));
            std::vector<std::pair<std::string, std::string>> received;
            const auto decoded = decoder.decode(stream_id, std::get<0>(section), collect, &received);
            RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).status == ruvia::Http3QpackDecodeStatus::kDecoded);
            RUVIA_CHECK_EQ(received.size(), 1U);
            if (received.size() == 1) {
                RUVIA_CHECK_EQ(received.front().first, std::string("x-sequence"));
                RUVIA_CHECK_EQ(received.front().second, value);
            }
            const auto acknowledgments = decoder.pendingDecoderOutput();
            RUVIA_CHECK(encoder.consumeDecoder(acknowledgments).index() == 0);
            RUVIA_CHECK(decoder.consumeDecoderOutput(acknowledgments.size()));
            RUVIA_CHECK_EQ(encoder.knownReceivedCount(), sequence + 1);
        }
    }
    RUVIA_CHECK(saw_wide_insert_count);
    RUVIA_CHECK(saw_wide_base);
    RUVIA_CHECK(saw_wrapped_insert_count);
    RUVIA_CHECK_EQ(result_resource.liveBytes, 0U);
}

RUVIA_TEST(http3_qpack_results_use_caller_resource_and_outlive_encoder) {
    int equality_group = 0;
    QpackResource encoder_resource(&equality_group);
    QpackResource result_resource(&equality_group);
    RUVIA_CHECK(&encoder_resource != &result_resource);
    RUVIA_CHECK(encoder_resource.is_equal(result_resource));
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "a value long enough to allocate"}};
    {
        std::pmr::vector<char> retained(&result_resource);
        std::vector<char> snapshot;
        {
            ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 0, .maxBlockedStreams = 0}, &encoder_resource);
            auto section = encoder.encode(0, fields, {}, &result_resource);
            RUVIA_CHECK((section).index() == 0);
            RUVIA_CHECK(std::get<0>(section).get_allocator().resource() == &result_resource);
            snapshot.assign(std::get<0>(section).begin(), std::get<0>(section).end());
            retained = std::move(std::get<0>(section));
        }
        RUVIA_CHECK(result_resource.liveBytes > 0);
        RUVIA_CHECK(!retained.empty());
        RUVIA_CHECK(std::equal(retained.begin(), retained.end(), snapshot.begin(), snapshot.end()));
    }
    RUVIA_CHECK_EQ(result_resource.liveBytes, 0u);
    RUVIA_CHECK_EQ(encoder_resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_dynamic_writer_result_uses_caller_resource) {
    int equality_group = 0;
    QpackResource encoder_resource(&equality_group);
    QpackResource result_resource(&equality_group);
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-name", "a value long enough to allocate"}};
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &encoder_resource);
        auto section = ruvia::encodeHttp3RequestTrailers(encoder, 0, fields, {}, &result_resource);
        RUVIA_CHECK((section).index() == 0);
        RUVIA_CHECK(std::get<0>(section).get_allocator().resource() == &result_resource);
        RUVIA_CHECK(result_resource.liveBytes > 0);
        RUVIA_CHECK_EQ(encoder.insertCount(), 1u);
    }
    RUVIA_CHECK_EQ(result_resource.liveBytes, 0u);
    RUVIA_CHECK_EQ(encoder_resource.liveBytes, 0u);
}
RUVIA_TEST(http3_qpack_result_allocation_failure_latches_terminal_error) {
    QpackResource encoder_resource;
    QpackResource result_resource;
    {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 128, .maxBlockedStreams = 1}, &encoder_resource);
        const std::array previous{ruvia::Http3FieldSectionFieldView{"x-prior", "value"}};
        const auto prior = encoder.encode(0, previous);
        RUVIA_CHECK((prior).index() == 0);
        const std::array fields{ruvia::Http3FieldSectionFieldView{
            "x-long-name-to-force-the-final-output-vector-to-allocate", "a sufficiently long value for the allocation"}};
        result_resource.fail = true;
        bool threw = false;
        try {
            (void)ruvia::detail::encodeHttp3Fields(fields, &result_resource, {}, &encoder, 0);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        result_resource.fail = false;
        const auto encode_error = encoder.encode(0, previous);
        RUVIA_CHECK((encode_error.index() != 0));
        RUVIA_CHECK(std::get<1>(encode_error) == ruvia::Http3QpackConnectionError::kDecoderStreamError);
        const std::array<char, 1> acknowledgment{static_cast<char>(0x80)};
        const auto decoder_error = encoder.consumeDecoder(acknowledgment);
        RUVIA_CHECK((decoder_error.index() != 0));
        RUVIA_CHECK(std::get<1>(decoder_error) == ruvia::Http3QpackConnectionError::kDecoderStreamError);
    }
    RUVIA_CHECK_EQ(result_resource.liveBytes, 0u);
    RUVIA_CHECK_EQ(encoder_resource.liveBytes, 0u);
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
        RUVIA_CHECK((decoder.consumeEncoder({&byte, 1})).index() == 0);
    }
    std::vector<std::pair<std::string, std::string>> received;
    const auto section = hex("03811011");
    RUVIA_CHECK((decoder.decode(4, section, collect, &received)).index() == 0);
    RUVIA_CHECK_EQ(received.size(), 2u);
    if (received.size() != 2) {
        return;
    }
    RUVIA_CHECK_EQ(received[0].first, std::string(":authority"));
    RUVIA_CHECK_EQ(received[0].second, std::string("www.example.com"));
    RUVIA_CHECK_EQ(received[1].second, std::string("/sample/path"));
    // Appendix B.3-B.4: literal name, duplicate, blocked relative references.
    RUVIA_CHECK((decoder.consumeEncoder(hex("4a637573746f6d2d6b65790c637573746f6d2d76616c7565"))).index() == 0);
    received.clear();
    const auto blocked = decoder.decode(8, hex("050080c181"), collect, &received);
    RUVIA_CHECK((blocked.index() == 0) && std::get<0>(blocked).status == ruvia::Http3QpackDecodeStatus::kBlocked);
    RUVIA_CHECK((decoder.consumeEncoder(hex("02"))).index() == 0);
    RUVIA_CHECK((decoder.decode(8, hex("050080c181"), collect, &received)).index() == 0);
    RUVIA_CHECK_EQ(received.size(), 3u);
    // Appendix B.5: insertion referencing a dynamic name and table eviction.
    RUVIA_CHECK((decoder.consumeEncoder(hex("810d637573746f6d2d76616c756532"))).index() == 0);
    received.clear();
    RUVIA_CHECK((decoder.decode(12, hex("060080"), collect, &received)).index() == 0);
    RUVIA_CHECK_EQ(received.size(), 1u);
    if (received.size() == 1) {
        RUVIA_CHECK_EQ(received[0].second, std::string("custom-value2"));
    }
}
