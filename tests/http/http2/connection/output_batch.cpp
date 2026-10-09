#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>

#include "http2/Http2OutputBuffer.h"
#include "test_harness.h"

namespace {

class FailingResource final : public std::pmr::memory_resource {
public:
    void fail(bool value) noexcept {
        fail_ = value;
        fail_after_.reset();
    }
    void fail_after(std::size_t allocations) noexcept {
        fail_after_ = allocations;
    }
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t deallocations() const noexcept {
        return deallocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail_ || (fail_after_ && allocations_ == *fail_after_)) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations_;
        return result;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool fail_{false};
    std::optional<std::size_t> fail_after_;
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

struct Observation final {
    std::uint32_t stream{0};
    std::size_t bytes{0};
    std::size_t calls{0};
};

void observe(void* context, std::uint32_t stream, std::size_t bytes) noexcept {
    auto& value = *static_cast<Observation*>(context);
    value.stream = stream;
    value.bytes += bytes;
    ++value.calls;
}

RUVIA_TEST(http2_output_batch_copy_failure_is_retryable) {
    FailingResource resource;
    ruvia::detail::Http2OutputBuffer output(&resource);
    output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "12345678");
    std::pmr::string target(&resource);
    target.reserve(1);
    resource.fail(true);
    bool threw = false;
    try {
        (void)output.takeBatch(1, target, nullptr, nullptr);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(output.pending().size(), std::size_t{17});
    resource.fail(false);
    const auto result = output.takeBatch(1, target, nullptr, nullptr);
    RUVIA_CHECK_EQ(result.status, ruvia::detail::Http2OutputBatchStatus::kTaken);
    RUVIA_CHECK_EQ(result.bytes, std::size_t{17});
}

RUVIA_TEST(http2_output_batch_soft_limit_and_data_stream_metadata) {
    ruvia::detail::Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendFrame(ruvia::Http2FrameType::kData, 0, 3, "abc");
    output.appendFrame(ruvia::Http2FrameType::kData, 0, 5, "wxyz");
    std::pmr::string target;
    Observation seen;
    const auto result = output.takeBatch(12, target, observe, &seen);
    RUVIA_CHECK_EQ(result.status, ruvia::detail::Http2OutputBatchStatus::kTaken);
    RUVIA_CHECK_EQ(result.bytes, std::size_t{12});
    RUVIA_CHECK_EQ(seen.calls, std::size_t{1});
    RUVIA_CHECK_EQ(seen.stream, std::uint32_t{3});
    RUVIA_CHECK_EQ(seen.bytes, std::size_t{3});
    RUVIA_CHECK_EQ(output.pendingDataBytes(5), std::size_t{4});
}

RUVIA_TEST(http2_output_batch_tracks_split_and_empty_payloads_after_partial_consumption) {
    ruvia::detail::Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendFrame(ruvia::Http2FrameType::kData, 0, 3, "ab", "cd");
    output.appendFrame(ruvia::Http2FrameType::kData, ruvia::detail::kHttp2FlagEndStream, 5, {});
    RUVIA_CHECK_EQ(output.pending().substr(ruvia::kHttp2FrameHeaderBytes, 4), "abcd");
    RUVIA_CHECK_EQ(output.pendingDataBytes(3), std::size_t{4});
    RUVIA_CHECK_EQ(output.pendingDataBytes(5), std::size_t{0});
    RUVIA_CHECK_EQ(output.consume(5), ruvia::Http2OutputConsumeStatus::kPending);
    RUVIA_CHECK_EQ(output.pendingDataBytes(3), std::size_t{4});
    RUVIA_CHECK_EQ(output.consume(6), ruvia::Http2OutputConsumeStatus::kPending);
    RUVIA_CHECK_EQ(output.pendingDataBytes(3), std::size_t{2});
    output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "12345678");
    RUVIA_CHECK_EQ(output.consume(2), ruvia::Http2OutputConsumeStatus::kPending);
    RUVIA_CHECK_EQ(output.pendingDataBytes(3), std::size_t{0});

    Observation seen;
    std::pmr::string batch;
    const auto empty_data = output.takeBatch(1, batch, observe, &seen);
    RUVIA_CHECK_EQ(empty_data.status, ruvia::Http2OutputBatchStatus::kTaken);
    RUVIA_CHECK_EQ(empty_data.bytes, ruvia::kHttp2FrameHeaderBytes);
    RUVIA_CHECK_EQ(seen.calls, std::size_t{1});
    RUVIA_CHECK_EQ(seen.stream, std::uint32_t{5});
    RUVIA_CHECK_EQ(seen.bytes, std::size_t{0});
    const auto ping = output.takeBatch(1, batch, observe, &seen);
    RUVIA_CHECK_EQ(ping.status, ruvia::Http2OutputBatchStatus::kTaken);
    RUVIA_CHECK_EQ(ping.bytes, std::size_t{17});
    RUVIA_CHECK_EQ(seen.calls, std::size_t{1});
    RUVIA_CHECK(!output.wantsWrite());
}

RUVIA_TEST(http2_output_batch_append_failure_preserves_pending_data_and_retry) {
    const auto seed = [&ruvia_ctx](ruvia::detail::Http2OutputBuffer& output, int mode) {
        if (mode == 0) {
            return;
        }
        for (int i = 0; i < 64; ++i) {
            output.appendFrame(ruvia::Http2FrameType::kData, 0, 9, "body");
        }
        // Exercise descriptor compaction both at a frame boundary and inside
        // the first remaining DATA payload, without byte-buffer compaction.
        const auto consumed = std::size_t{40 * 13} + (mode == 2 ? 10 : 0);
        RUVIA_CHECK_EQ(output.consume(consumed), ruvia::Http2OutputConsumeStatus::kPending);
    };
    const auto append = [](ruvia::detail::Http2OutputBuffer& output, int index) {
        if (index % 2 == 0) {
            output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "12345678");
        } else {
            output.appendFrame(ruvia::Http2FrameType::kData, 0, 9, "body");
        }
    };
    for (int mode = 0; mode < 3; ++mode) {
        FailingResource baseline_resource;
        std::size_t append_allocations = 0;
        std::string expected;
        {
            ruvia::detail::Http2OutputBuffer output(&baseline_resource);
            seed(output, mode);
            const auto before = baseline_resource.allocations();
            for (int i = 0; i < 64; ++i) {
                append(output, i);
            }
            append_allocations = baseline_resource.allocations() - before;
            expected = output.pending();
        }
        RUVIA_CHECK(append_allocations != 0);
        RUVIA_CHECK_EQ(baseline_resource.allocations(), baseline_resource.deallocations());
        for (std::size_t allocation = 0; allocation < append_allocations; ++allocation) {
            FailingResource resource;
            {
                ruvia::detail::Http2OutputBuffer output(&resource);
                seed(output, mode);
                resource.fail_after(resource.allocations() + allocation);
                bool threw = false;
                for (int i = 0; i < 64; ++i) {
                    const auto before = std::string(output.pending());
                    const auto data_before = output.pendingDataBytes(9);
                    const auto checkpoint = output.checkpoint();
                    try {
                        append(output, i);
                    } catch (const std::bad_alloc&) {
                        RUVIA_CHECK(!threw);
                        threw = true;
                        RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
                        RUVIA_CHECK_EQ(output.pendingDataBytes(9), data_before);
                        RUVIA_CHECK_EQ(output.checkpoint(), checkpoint);
                        resource.fail(false);
                        append(output, i);
                    }
                }
                RUVIA_CHECK(threw);
                RUVIA_CHECK_EQ(output.pending(), std::string_view(expected));
                if (mode == 2) {
                    // Complete the partially consumed DATA frame before taking
                    // whole-frame batches; its remaining payload is three bytes.
                    RUVIA_CHECK_EQ(output.pendingDataBytes(9), std::size_t{223});
                    RUVIA_CHECK_EQ(output.consume(3), ruvia::Http2OutputConsumeStatus::kPending);
                }
                Observation seen;
                std::pmr::string batch(&resource);
                while (output.wantsWrite()) {
                    const auto result = output.takeBatch(37, batch, observe, &seen);
                    RUVIA_CHECK_EQ(result.status, ruvia::Http2OutputBatchStatus::kTaken);
                    RUVIA_CHECK(result.bytes != 0);
                    if (result.status != ruvia::Http2OutputBatchStatus::kTaken) {
                        break;
                    }
                }
                RUVIA_CHECK_EQ(std::string_view(batch), std::string_view(expected).substr(mode == 2 ? 3 : 0));
                const auto data_frames = std::size_t{32} + (mode == 0 ? 0 : mode == 1 ? 24
                                                                                      : 23);
                RUVIA_CHECK_EQ(seen.calls, data_frames);
                RUVIA_CHECK_EQ(seen.bytes, data_frames * 4);
                RUVIA_CHECK_EQ(seen.stream, std::uint32_t{9});
                RUVIA_CHECK_EQ(output.pendingDataBytes(9), std::size_t{0});
            }
            RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
        }
    }
}

RUVIA_TEST(http2_output_batch_compacts_long_lived_interleaved_output) {
    FailingResource resource;
    {
        ruvia::detail::Http2OutputBuffer output(&resource);
        std::string payload(40 * 1024, 'a');
        std::pmr::string batch(&resource);
        for (int i = 0; i < 200; ++i) {
            payload.front() = static_cast<char>('a' + i % 26);
            output.appendFrame(ruvia::Http2FrameType::kData, 0, i == 0 ? 11 : 12, payload);
            if (i == 0) {
                output.appendFrame(ruvia::Http2FrameType::kData, 0, 12, payload);
            }
            const auto firstSize = ruvia::kHttp2FrameHeaderBytes + payload.size();
            const auto result = output.takeBatch(firstSize, batch, nullptr, nullptr);
            RUVIA_CHECK_EQ(result.bytes, firstSize);
            batch.clear();
            const auto pending = output.pending();
            RUVIA_CHECK_EQ(pending.size(), firstSize);
            RUVIA_CHECK_EQ(static_cast<unsigned char>(pending[ruvia::kHttp2FrameHeaderBytes]),
                static_cast<unsigned char>(payload.front()));
            RUVIA_CHECK_EQ(output.pendingDataBytes(12), payload.size());
            const auto expectedPending = std::string(pending);
            const auto checkpoint = output.checkpoint();
            output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "12345678");
            output.rollbackTo(checkpoint);
            RUVIA_CHECK_EQ(output.pending(), std::string_view(expectedPending));
        }
    }
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(http2_output_checkpoint_tracks_logical_end_after_partial_drain) {
    ruvia::detail::Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "12345678");
    RUVIA_CHECK_EQ(output.consume(5), ruvia::detail::Http2OutputConsumeStatus::kPending);

    const auto checkpoint = output.checkpoint();
    output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "abcdefgh");
    output.rollbackTo(checkpoint);
    RUVIA_CHECK_EQ(output.pending().size(), std::size_t{12});
    RUVIA_CHECK_EQ(output.consume(12), ruvia::detail::Http2OutputConsumeStatus::kDrained);
    RUVIA_CHECK_EQ(output.checkpoint(), checkpoint);

    output.appendFrame(ruvia::Http2FrameType::kPing, 0, 0, "abcdefgh");
    RUVIA_CHECK_EQ(output.checkpoint(), checkpoint + std::size_t{17});
}

RUVIA_TEST(http2_output_batch_rejects_legacy_mid_frame_cursor) {
    ruvia::detail::Http2OutputBuffer output(std::pmr::get_default_resource());
    output.appendFrame(ruvia::Http2FrameType::kData, 0, 7, "payload");
    RUVIA_CHECK_EQ(output.consume(10), ruvia::Http2OutputConsumeStatus::kPending);
    RUVIA_CHECK_EQ(output.pendingDataBytes(7), std::size_t{6});
    std::pmr::string target;
    Observation seen;
    const auto result = output.takeBatch(100, target, observe, &seen);
    RUVIA_CHECK_EQ(result.status, ruvia::detail::Http2OutputBatchStatus::kUnaligned);
    RUVIA_CHECK(target.empty());
    RUVIA_CHECK_EQ(seen.calls, std::size_t{0});
}

}  // namespace
