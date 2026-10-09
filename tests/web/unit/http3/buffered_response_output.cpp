#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"

#include "http3/http3_buffered_response_output.h"
#include "test_harness.h"

namespace {

using output_type = ruvia::detail::http3_buffered_response_output;
using buffer = ruvia::detail::http3_stream_buffer;
using message_id_type = ruvia::detail::http3_stream_id;
using control_type = ruvia::detail::http3_stream_control;

class watchdog final {
public:
    watchdog()
        : thread_([this] {
              std::unique_lock lock(mutex_);
              if (!condition_.wait_for(lock, std::chrono::seconds(5), [this] {
                      return done_;
                  })) {
                  std::terminate();
              }
          }) {}

    ~watchdog() {
        {
            const std::lock_guard lock(mutex_);
            done_ = true;
        }
        condition_.notify_one();
        thread_.join();
    }

    watchdog(const watchdog&) = delete;
    watchdog& operator=(const watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_{};
    std::thread thread_;
};

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns_;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

constexpr message_id_type message_id{.epoch_ = 11, .connection_generation_ = 17, .stream_id_ = 4};

output_type make_output(const ruvia::http_response& response,
    const ruvia::http_buffered_response_write_plan& plan, ruvia::worker_memory& worker_value,
    buffer& buffer, std::optional<std::uint64_t> peer_limit = std::nullopt) {
    auto output = output_type::create(response, plan, worker_value, buffer, message_id, peer_limit);
    if ((output.index() != 0)) {
        throw std::runtime_error("failed to create buffered HTTP/3 response output");
    }
    return std::move(std::get<0>(output));
}

void collect_one(buffer& buffer, std::string& wire, ruvia::testing::test_context& ruvia_ctx,
    std::optional<std::size_t> expected_size = std::nullopt) {
    buffer::borrowed_block block;
    const bool received_value = buffer.try_receive(block);
    RUVIA_CHECK(received_value);
    if (!received_value) {
        return;
    }
    RUVIA_CHECK(block.id().epoch_ == message_id.epoch_);
    RUVIA_CHECK(block.id().connection_generation_ == message_id.connection_generation_);
    RUVIA_CHECK(block.id().stream_id_ == message_id.stream_id_);
    const auto bytes_value = block.bytes();
    if (expected_size) {
        RUVIA_CHECK_EQ(bytes_value.size(), *expected_size);
    }
    wire.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
    block.release();
    RUVIA_CHECK(!buffer.has_pending());
}

RUVIA_TEST(http3_buffered_response_output_publishes_empty_response_headers_then_exact_fin) {
    watchdog watchdog;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::http_response response;
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto output = make_output(response, plan, worker, buffer);
        std::string wire;

        RUVIA_CHECK(output.next_step() == output_type::next_step_type::bytes);
        const auto decoded_field_section_size = output.decoded_field_section_size();
        RUVIA_CHECK(decoded_field_section_size > 0);
        const auto headers = output.publish_step();
        RUVIA_CHECK(headers.status_ == output_type::status_type::bytes);
        RUVIA_CHECK(headers.bytes_accepted_ > 0);
        RUVIA_CHECK_EQ(headers.published_wire_bytes_, headers.bytes_accepted_);
        collect_one(buffer, wire, ruvia_ctx);
        RUVIA_CHECK(output.next_step() == output_type::next_step_type::fin);
        RUVIA_CHECK_EQ(output.decoded_field_section_size(), decoded_field_section_size);

        const control_type blocker{control_type::kind::writable, message_id};
        RUVIA_CHECK(buffer.try_send_control(blocker) == buffer::control_result::sent);
        const auto blocked_fin = output.publish_step();
        RUVIA_CHECK(blocked_fin.status_ == output_type::status_type::backpressured);
        RUVIA_CHECK(blocked_fin.block_reason_ == output_type::block_reason_type::control);
        RUVIA_CHECK_EQ(blocked_fin.published_wire_bytes_, headers.published_wire_bytes_);
        control_type blocker_received;
        RUVIA_CHECK(buffer.try_receive_control(blocker_received));
        RUVIA_CHECK(blocker_received.kind_ == control_type::kind::writable);
        RUVIA_CHECK(!buffer.has_pending());

        const auto fin_result = output.publish_step();
        RUVIA_CHECK(fin_result.status_ == output_type::status_type::fin);
        RUVIA_CHECK_EQ(fin_result.bytes_accepted_, 0U);
        RUVIA_CHECK_EQ(fin_result.published_wire_bytes_, wire.size());
        RUVIA_CHECK(output.complete());
        RUVIA_CHECK(output.next_step() == output_type::next_step_type::complete);
        RUVIA_CHECK(!output.failed());

        control_type fin;
        RUVIA_CHECK(buffer.try_receive_control(fin));
        RUVIA_CHECK(fin.kind_ == control_type::kind::stream_fin);
        RUVIA_CHECK(fin.id_.epoch_ == message_id.epoch_);
        RUVIA_CHECK(fin.id_.connection_generation_ == message_id.connection_generation_);
        RUVIA_CHECK(fin.id_.stream_id_ == message_id.stream_id_);
        RUVIA_CHECK_EQ(fin.value_, wire.size());
        RUVIA_CHECK_EQ(output.published_wire_bytes(), fin.value_);
        RUVIA_CHECK(!buffer.has_pending());
        RUVIA_CHECK(output.publish_step().status_ == output_type::status_type::complete);
        RUVIA_CHECK(upstream.allocations_ > upstream.returns_);
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
}

RUVIA_TEST(http3_buffered_response_output_resumes_after_data_backpressure_and_publishes_partial_block) {
    watchdog watchdog;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::http_response response;
        const std::string body(buffer::max_block_bytes + 37, 'b');
        response.body(body);
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto output = make_output(response, plan, worker, buffer);
        std::string wire;

        const auto headers = output.publish_step();
        RUVIA_CHECK(headers.status_ == output_type::status_type::bytes);
        collect_one(buffer, wire, ruvia_ctx);

        const auto data_frame = output.publish_step();
        RUVIA_CHECK(data_frame.status_ == output_type::status_type::bytes);
        const auto blocked_on_frame = output.publish_step();
        RUVIA_CHECK(blocked_on_frame.status_ == output_type::status_type::backpressured);
        RUVIA_CHECK(blocked_on_frame.block_reason_ == output_type::block_reason_type::data);
        RUVIA_CHECK_EQ(blocked_on_frame.published_wire_bytes_, data_frame.published_wire_bytes_);
        collect_one(buffer, wire, ruvia_ctx);

        const auto body_block = output.publish_step();
        RUVIA_CHECK(body_block.status_ == output_type::status_type::bytes);
        RUVIA_CHECK_EQ(body_block.bytes_accepted_, buffer::max_block_bytes);
        const auto blocked_on_body = output.publish_step();
        RUVIA_CHECK(blocked_on_body.status_ == output_type::status_type::backpressured);
        RUVIA_CHECK(blocked_on_body.block_reason_ == output_type::block_reason_type::data);
        RUVIA_CHECK_EQ(blocked_on_body.published_wire_bytes_, body_block.published_wire_bytes_);
        collect_one(buffer, wire, ruvia_ctx, buffer::max_block_bytes);

        const auto body_remainder = output.publish_step();
        RUVIA_CHECK(body_remainder.status_ == output_type::status_type::bytes);
        RUVIA_CHECK_EQ(body_remainder.bytes_accepted_, 37U);
        collect_one(buffer, wire, ruvia_ctx, 37U);
        RUVIA_CHECK_EQ(output.published_wire_bytes(), wire.size());

        const auto fin_result = output.publish_step();
        RUVIA_CHECK(fin_result.status_ == output_type::status_type::fin);
        RUVIA_CHECK_EQ(fin_result.published_wire_bytes_, wire.size());
        control_type fin;
        RUVIA_CHECK(buffer.try_receive_control(fin));
        RUVIA_CHECK_EQ(fin.value_, wire.size());
        RUVIA_CHECK(buffer.has_pending() == false);
        RUVIA_CHECK(output.complete());
        RUVIA_CHECK(upstream.allocations_ > upstream.returns_);
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
}

RUVIA_TEST(http3_buffered_response_output_rejects_peer_field_limit_before_publishing_headers) {
    watchdog watchdog;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::http_response response;
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto output = output_type::create(response, plan, worker, buffer, message_id, 0);
        RUVIA_CHECK((output.index() != 0));
        if ((output.index() != 0)) {
            RUVIA_CHECK(std::get<1>(output) == output_type::error_type::peer_field_section_limit);
        }
        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!buffer.try_receive(block));
        RUVIA_CHECK(!buffer.try_receive_control(control));
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
}

RUVIA_TEST(http3_buffered_response_output_preserves_published_bytes_across_buffer_stop) {
    watchdog watchdog;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::http_response response;
        response.body("accepted before stop");
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto output = make_output(response, plan, worker, buffer);

        const auto published = output.publish_step();
        RUVIA_CHECK(published.status_ == output_type::status_type::bytes);
        RUVIA_CHECK_EQ(published.published_wire_bytes_, published.bytes_accepted_);
        RUVIA_CHECK(!output.complete());

        RUVIA_CHECK(buffer.stop());
        const auto stopped = output.publish_step();
        RUVIA_CHECK(stopped.status_ == output_type::status_type::failed);
        RUVIA_CHECK(stopped.error_ == output_type::error_type::buffer_stopped);
        RUVIA_CHECK_EQ(stopped.published_wire_bytes_, published.published_wire_bytes_);
        RUVIA_CHECK(output.failed());
        RUVIA_CHECK(!output.complete());
        RUVIA_CHECK(output.publish_step().status_ == output_type::status_type::failed);

        std::string wire;
        collect_one(buffer, wire, ruvia_ctx);
        RUVIA_CHECK_EQ(wire.size(), published.bytes_accepted_);
        RUVIA_CHECK(upstream.allocations_ > upstream.returns_);
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
}

RUVIA_TEST(http3_buffered_response_output_explicit_stop_never_claims_completion) {
    watchdog watchdog;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::http_response response;
        response.body("not published");
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto output = make_output(response, plan, worker, buffer);

        output.stop();
        RUVIA_CHECK(output.failed());
        RUVIA_CHECK(!output.complete());
        const auto stopped = output.publish_step();
        RUVIA_CHECK(stopped.status_ == output_type::status_type::failed);
        RUVIA_CHECK(stopped.error_ == output_type::error_type::stopped);
        RUVIA_CHECK_EQ(stopped.published_wire_bytes_, 0U);
        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!buffer.try_receive(block));
        RUVIA_CHECK(!buffer.try_receive_control(control));
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
}

}  // namespace
