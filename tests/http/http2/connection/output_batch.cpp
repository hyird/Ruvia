#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>

#include "http2/http2_output_buffer.h"
#include "test_harness.h"

namespace {

class failing_resource final : public std::pmr::memory_resource {
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
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_ || (fail_after_ && allocations_ == *fail_after_)) {
            throw std::bad_alloc();
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        return result_value;
    }
    void do_deallocate(void* p, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(p, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool fail_{false};
    std::optional<std::size_t> fail_after_;
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

struct observation final {
    std::uint32_t stream_{0};
    std::size_t bytes_{0};
    std::size_t calls_{0};
};

void observe(void* context_value, std::uint32_t stream, std::size_t bytes_value) noexcept {
    auto& value = *static_cast<observation*>(context_value);
    value.stream_ = stream;
    value.bytes_ += bytes_value;
    ++value.calls_;
}

RUVIA_TEST(http2_output_batch_copy_failure_is_retryable) {
    failing_resource resource;
    ruvia::detail::http2_output_buffer output(&resource);
    output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "12345678");
    std::pmr::string target(&resource);
    target.reserve(1);
    resource.fail(true);
    bool threw = false;
    try {
        (void)output.take_batch(1, target, nullptr, nullptr);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(output.pending().size(), std::size_t{17});
    resource.fail(false);
    const auto result_value = output.take_batch(1, target, nullptr, nullptr);
    RUVIA_CHECK_EQ(result_value.status_, ruvia::detail::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(result_value.bytes_, std::size_t{17});
}

RUVIA_TEST(http2_output_batch_soft_limit_and_data_stream_metadata) {
    ruvia::detail::http2_output_buffer output(std::pmr::get_default_resource());
    output.append_frame(ruvia::http2_frame_type::data, 0, 3, "abc");
    output.append_frame(ruvia::http2_frame_type::data, 0, 5, "wxyz");
    std::pmr::string target;
    observation seen;
    const auto result_value = output.take_batch(12, target, observe, &seen);
    RUVIA_CHECK_EQ(result_value.status_, ruvia::detail::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(result_value.bytes_, std::size_t{12});
    RUVIA_CHECK_EQ(seen.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(seen.stream_, std::uint32_t{3});
    RUVIA_CHECK_EQ(seen.bytes_, std::size_t{3});
    RUVIA_CHECK_EQ(output.pending_data_bytes(5), std::size_t{4});
}

RUVIA_TEST(http2_output_batch_tracks_split_and_empty_payloads_after_partial_consumption) {
    ruvia::detail::http2_output_buffer output(std::pmr::get_default_resource());
    output.append_frame(ruvia::http2_frame_type::data, 0, 3, "ab", "cd");
    output.append_frame(ruvia::http2_frame_type::data, ruvia::detail::http2_flag_end_stream, 5, {});
    RUVIA_CHECK_EQ(output.pending().substr(ruvia::http2_frame_header_bytes, 4), "abcd");
    RUVIA_CHECK_EQ(output.pending_data_bytes(3), std::size_t{4});
    RUVIA_CHECK_EQ(output.pending_data_bytes(5), std::size_t{0});
    RUVIA_CHECK_EQ(output.consume(5), ruvia::http2_output_consume_status::pending);
    RUVIA_CHECK_EQ(output.pending_data_bytes(3), std::size_t{4});
    RUVIA_CHECK_EQ(output.consume(6), ruvia::http2_output_consume_status::pending);
    RUVIA_CHECK_EQ(output.pending_data_bytes(3), std::size_t{2});
    output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "12345678");
    RUVIA_CHECK_EQ(output.consume(2), ruvia::http2_output_consume_status::pending);
    RUVIA_CHECK_EQ(output.pending_data_bytes(3), std::size_t{0});

    observation seen;
    std::pmr::string batch;
    const auto empty_data = output.take_batch(1, batch, observe, &seen);
    RUVIA_CHECK_EQ(empty_data.status_, ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(empty_data.bytes_, ruvia::http2_frame_header_bytes);
    RUVIA_CHECK_EQ(seen.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(seen.stream_, std::uint32_t{5});
    RUVIA_CHECK_EQ(seen.bytes_, std::size_t{0});
    const auto ping = output.take_batch(1, batch, observe, &seen);
    RUVIA_CHECK_EQ(ping.status_, ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(ping.bytes_, std::size_t{17});
    RUVIA_CHECK_EQ(seen.calls_, std::size_t{1});
    RUVIA_CHECK(!output.wants_write());
}

RUVIA_TEST(http2_output_batch_append_failure_preserves_pending_data_and_retry) {
    const auto seed = [&ruvia_ctx](ruvia::detail::http2_output_buffer& output, int mode) {
        if (mode == 0) {
            return;
        }
        for (int i = 0; i < 64; ++i) {
            output.append_frame(ruvia::http2_frame_type::data, 0, 9, "body");
        }
        // Exercise descriptor compaction both at a frame boundary and inside
        // the first remaining DATA payload, without byte-buffer compaction.
        const auto consumed = std::size_t{40 * 13} + (mode == 2 ? 10 : 0);
        RUVIA_CHECK_EQ(output.consume(consumed), ruvia::http2_output_consume_status::pending);
    };
    const auto append = [](ruvia::detail::http2_output_buffer& output, int index) {
        if (index % 2 == 0) {
            output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "12345678");
        } else {
            output.append_frame(ruvia::http2_frame_type::data, 0, 9, "body");
        }
    };
    for (int mode = 0; mode < 3; ++mode) {
        failing_resource baseline_resource;
        std::size_t append_allocations = 0;
        std::string expected;
        {
            ruvia::detail::http2_output_buffer output(&baseline_resource);
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
            failing_resource resource;
            {
                ruvia::detail::http2_output_buffer output(&resource);
                seed(output, mode);
                resource.fail_after(resource.allocations() + allocation);
                bool threw = false;
                for (int i = 0; i < 64; ++i) {
                    const auto before = std::string(output.pending());
                    const auto data_before = output.pending_data_bytes(9);
                    const auto checkpoint = output.checkpoint();
                    try {
                        append(output, i);
                    } catch (const std::bad_alloc&) {
                        RUVIA_CHECK(!threw);
                        threw = true;
                        RUVIA_CHECK_EQ(output.pending(), std::string_view(before));
                        RUVIA_CHECK_EQ(output.pending_data_bytes(9), data_before);
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
                    RUVIA_CHECK_EQ(output.pending_data_bytes(9), std::size_t{223});
                    RUVIA_CHECK_EQ(output.consume(3), ruvia::http2_output_consume_status::pending);
                }
                observation seen;
                std::pmr::string batch(&resource);
                while (output.wants_write()) {
                    const auto result_value = output.take_batch(37, batch, observe, &seen);
                    RUVIA_CHECK_EQ(result_value.status_, ruvia::http2_output_batch_status::taken);
                    RUVIA_CHECK(result_value.bytes_ != 0);
                    if (result_value.status_ != ruvia::http2_output_batch_status::taken) {
                        break;
                    }
                }
                RUVIA_CHECK_EQ(std::string_view(batch), std::string_view(expected).substr(mode == 2 ? 3 : 0));
                const auto data_frames = std::size_t{32} + (mode == 0 ? 0 : mode == 1 ? 24
                                                                                      : 23);
                RUVIA_CHECK_EQ(seen.calls_, data_frames);
                RUVIA_CHECK_EQ(seen.bytes_, data_frames * 4);
                RUVIA_CHECK_EQ(seen.stream_, std::uint32_t{9});
                RUVIA_CHECK_EQ(output.pending_data_bytes(9), std::size_t{0});
            }
            RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
        }
    }
}

RUVIA_TEST(http2_output_batch_compacts_long_lived_interleaved_output) {
    failing_resource resource;
    {
        ruvia::detail::http2_output_buffer output(&resource);
        std::string payload_value(40 * 1024, 'a');
        std::pmr::string batch(&resource);
        for (int i = 0; i < 200; ++i) {
            payload_value.front() = static_cast<char>('a' + i % 26);
            output.append_frame(ruvia::http2_frame_type::data, 0, i == 0 ? 11 : 12, payload_value);
            if (i == 0) {
                output.append_frame(ruvia::http2_frame_type::data, 0, 12, payload_value);
            }
            const auto first_size = ruvia::http2_frame_header_bytes + payload_value.size();
            const auto result_value = output.take_batch(first_size, batch, nullptr, nullptr);
            RUVIA_CHECK_EQ(result_value.bytes_, first_size);
            batch.clear();
            const auto pending = output.pending();
            RUVIA_CHECK_EQ(pending.size(), first_size);
            RUVIA_CHECK_EQ(static_cast<unsigned char>(pending[ruvia::http2_frame_header_bytes]),
                static_cast<unsigned char>(payload_value.front()));
            RUVIA_CHECK_EQ(output.pending_data_bytes(12), payload_value.size());
            const auto expected_pending = std::string(pending);
            const auto checkpoint = output.checkpoint();
            output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "12345678");
            output.rollback_to(checkpoint);
            RUVIA_CHECK_EQ(output.pending(), std::string_view(expected_pending));
        }
    }
    RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
}

RUVIA_TEST(http2_output_checkpoint_tracks_logical_end_after_partial_drain) {
    ruvia::detail::http2_output_buffer output(std::pmr::get_default_resource());
    output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "12345678");
    RUVIA_CHECK_EQ(output.consume(5), ruvia::detail::http2_output_consume_status::pending);

    const auto checkpoint = output.checkpoint();
    output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "abcdefgh");
    output.rollback_to(checkpoint);
    RUVIA_CHECK_EQ(output.pending().size(), std::size_t{12});
    RUVIA_CHECK_EQ(output.consume(12), ruvia::detail::http2_output_consume_status::drained);
    RUVIA_CHECK_EQ(output.checkpoint(), checkpoint);

    output.append_frame(ruvia::http2_frame_type::ping, 0, 0, "abcdefgh");
    RUVIA_CHECK_EQ(output.checkpoint(), checkpoint + std::size_t{17});
}

RUVIA_TEST(http2_output_batch_rejects_legacy_mid_frame_cursor) {
    ruvia::detail::http2_output_buffer output(std::pmr::get_default_resource());
    output.append_frame(ruvia::http2_frame_type::data, 0, 7, "payload");
    RUVIA_CHECK_EQ(output.consume(10), ruvia::http2_output_consume_status::pending);
    RUVIA_CHECK_EQ(output.pending_data_bytes(7), std::size_t{6});
    std::pmr::string target;
    observation seen;
    const auto result_value = output.take_batch(100, target, observe, &seen);
    RUVIA_CHECK_EQ(result_value.status_, ruvia::detail::http2_output_batch_status::unaligned);
    RUVIA_CHECK(target.empty());
    RUVIA_CHECK_EQ(seen.calls_, std::size_t{0});
}

}  // namespace
