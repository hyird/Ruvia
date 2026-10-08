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

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/http3/Http3BufferedResponseOutput.h"

#include "test_harness.h"

namespace {

using Output = ruvia::detail::Http3BufferedResponseOutput;
using buffer = ruvia::detail::http3_stream_buffer;
using MessageId = ruvia::detail::http3_stream_id;
using Control = ruvia::detail::http3_stream_control;

class Watchdog final {
public:
    Watchdog()
        : thread_([this] {
              std::unique_lock lock(mutex_);
              if (!condition_.wait_for(lock, std::chrono::seconds(5), [this] {
                      return done_;
                  })) {
                  std::terminate();
              }
          }) {}

    ~Watchdog() {
        {
            const std::lock_guard lock(mutex_);
            done_ = true;
        }
        condition_.notify_one();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_{};
    std::thread thread_;
};

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

constexpr MessageId kMessageId{.epoch = 11, .connection_generation = 17, .stream_id = 4};

Output makeOutput(const ruvia::HttpResponse& response,
    const ruvia::HttpBufferedResponseWritePlan& plan, ruvia::WorkerMemory& worker,
    buffer& buffer, std::optional<std::uint64_t> peerLimit = std::nullopt) {
    auto output = Output::create(response, plan, worker, buffer, kMessageId, peerLimit);
    if (!output) {
        throw std::runtime_error("failed to create buffered HTTP/3 response output");
    }
    return std::move(*output);
}

void collectOne(buffer& buffer, std::string& wire, ruvia::testing::TestContext& ruvia_ctx,
    std::optional<std::size_t> expectedSize = std::nullopt) {
    buffer::borrowed_block block;
    const bool received = buffer.try_receive(block);
    RUVIA_CHECK(received);
    if (!received) {
        return;
    }
    RUVIA_CHECK(block.id().epoch == kMessageId.epoch);
    RUVIA_CHECK(block.id().connection_generation == kMessageId.connection_generation);
    RUVIA_CHECK(block.id().stream_id == kMessageId.stream_id);
    const auto bytes = block.bytes();
    if (expectedSize) {
        RUVIA_CHECK_EQ(bytes.size(), *expectedSize);
    }
    wire.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    block.release();
    RUVIA_CHECK(!buffer.has_pending());
}

RUVIA_TEST(http3BufferedResponseOutputPublishesEmptyResponseHeadersThenExactFin) {
    Watchdog watchdog;
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::HttpResponse response;
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto output = makeOutput(response, plan, worker, buffer);
        std::string wire;

        RUVIA_CHECK(output.nextStep() == Output::NextStep::bytes);
        const auto decodedFieldSectionSize = output.decodedFieldSectionSize();
        RUVIA_CHECK(decodedFieldSectionSize > 0);
        const auto headers = output.publishStep();
        RUVIA_CHECK(headers.status == Output::Status::kBytes);
        RUVIA_CHECK(headers.bytesAccepted > 0);
        RUVIA_CHECK_EQ(headers.publishedWireBytes, headers.bytesAccepted);
        collectOne(buffer, wire, ruvia_ctx);
        RUVIA_CHECK(output.nextStep() == Output::NextStep::fin);
        RUVIA_CHECK_EQ(output.decodedFieldSectionSize(), decodedFieldSectionSize);

        const Control blocker{Control::kind::writable, kMessageId};
        RUVIA_CHECK(buffer.try_send_control(blocker) == buffer::control_result::sent);
        const auto blockedFin = output.publishStep();
        RUVIA_CHECK(blockedFin.status == Output::Status::kBackpressured);
        RUVIA_CHECK(blockedFin.blockReason == Output::BlockReason::kControl);
        RUVIA_CHECK_EQ(blockedFin.publishedWireBytes, headers.publishedWireBytes);
        Control blockerReceived;
        RUVIA_CHECK(buffer.try_receive_control(blockerReceived));
        RUVIA_CHECK(blockerReceived.kind == Control::kind::writable);
        RUVIA_CHECK(!buffer.has_pending());

        const auto finResult = output.publishStep();
        RUVIA_CHECK(finResult.status == Output::Status::kFin);
        RUVIA_CHECK_EQ(finResult.bytesAccepted, 0U);
        RUVIA_CHECK_EQ(finResult.publishedWireBytes, wire.size());
        RUVIA_CHECK(output.complete());
        RUVIA_CHECK(output.nextStep() == Output::NextStep::complete);
        RUVIA_CHECK(!output.failed());

        Control fin;
        RUVIA_CHECK(buffer.try_receive_control(fin));
        RUVIA_CHECK(fin.kind == Control::kind::stream_fin);
        RUVIA_CHECK(fin.id.epoch == kMessageId.epoch);
        RUVIA_CHECK(fin.id.connection_generation == kMessageId.connection_generation);
        RUVIA_CHECK(fin.id.stream_id == kMessageId.stream_id);
        RUVIA_CHECK_EQ(fin.value, wire.size());
        RUVIA_CHECK_EQ(output.publishedWireBytes(), fin.value);
        RUVIA_CHECK(!buffer.has_pending());
        RUVIA_CHECK(output.publishStep().status == Output::Status::kComplete);
        RUVIA_CHECK(upstream.allocations > upstream.returns);
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
}

RUVIA_TEST(http3BufferedResponseOutputResumesAfterDataBackpressureAndPublishesPartialBlock) {
    Watchdog watchdog;
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::HttpResponse response;
        const std::string body(buffer::max_block_bytes + 37, 'b');
        response.body(body);
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto output = makeOutput(response, plan, worker, buffer);
        std::string wire;

        const auto headers = output.publishStep();
        RUVIA_CHECK(headers.status == Output::Status::kBytes);
        collectOne(buffer, wire, ruvia_ctx);

        const auto dataFrame = output.publishStep();
        RUVIA_CHECK(dataFrame.status == Output::Status::kBytes);
        const auto blockedOnFrame = output.publishStep();
        RUVIA_CHECK(blockedOnFrame.status == Output::Status::kBackpressured);
        RUVIA_CHECK(blockedOnFrame.blockReason == Output::BlockReason::kData);
        RUVIA_CHECK_EQ(blockedOnFrame.publishedWireBytes, dataFrame.publishedWireBytes);
        collectOne(buffer, wire, ruvia_ctx);

        const auto bodyBlock = output.publishStep();
        RUVIA_CHECK(bodyBlock.status == Output::Status::kBytes);
        RUVIA_CHECK_EQ(bodyBlock.bytesAccepted, buffer::max_block_bytes);
        const auto blockedOnBody = output.publishStep();
        RUVIA_CHECK(blockedOnBody.status == Output::Status::kBackpressured);
        RUVIA_CHECK(blockedOnBody.blockReason == Output::BlockReason::kData);
        RUVIA_CHECK_EQ(blockedOnBody.publishedWireBytes, bodyBlock.publishedWireBytes);
        collectOne(buffer, wire, ruvia_ctx, buffer::max_block_bytes);

        const auto bodyRemainder = output.publishStep();
        RUVIA_CHECK(bodyRemainder.status == Output::Status::kBytes);
        RUVIA_CHECK_EQ(bodyRemainder.bytesAccepted, 37U);
        collectOne(buffer, wire, ruvia_ctx, 37U);
        RUVIA_CHECK_EQ(output.publishedWireBytes(), wire.size());

        const auto finResult = output.publishStep();
        RUVIA_CHECK(finResult.status == Output::Status::kFin);
        RUVIA_CHECK_EQ(finResult.publishedWireBytes, wire.size());
        Control fin;
        RUVIA_CHECK(buffer.try_receive_control(fin));
        RUVIA_CHECK_EQ(fin.value, wire.size());
        RUVIA_CHECK(buffer.has_pending() == false);
        RUVIA_CHECK(output.complete());
        RUVIA_CHECK(upstream.allocations > upstream.returns);
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
}

RUVIA_TEST(http3BufferedResponseOutputRejectsPeerFieldLimitBeforePublishingHeaders) {
    Watchdog watchdog;
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::HttpResponse response;
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto output = Output::create(response, plan, worker, buffer, kMessageId, 0);
        RUVIA_CHECK(!output);
        if (!output) {
            RUVIA_CHECK(output.error() == Output::Error::kPeerFieldSectionLimit);
        }
        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!buffer.try_receive(block));
        RUVIA_CHECK(!buffer.try_receive_control(control));
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
}

RUVIA_TEST(http3_buffered_response_output_preserves_published_bytes_across_buffer_stop) {
    Watchdog watchdog;
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::HttpResponse response;
        response.body("accepted before stop");
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto output = makeOutput(response, plan, worker, buffer);

        const auto published = output.publishStep();
        RUVIA_CHECK(published.status == Output::Status::kBytes);
        RUVIA_CHECK_EQ(published.publishedWireBytes, published.bytesAccepted);
        RUVIA_CHECK(!output.complete());

        RUVIA_CHECK(buffer.stop());
        const auto stopped = output.publishStep();
        RUVIA_CHECK(stopped.status == Output::Status::kFailed);
        RUVIA_CHECK(stopped.error == Output::Error::buffer_stopped);
        RUVIA_CHECK_EQ(stopped.publishedWireBytes, published.publishedWireBytes);
        RUVIA_CHECK(output.failed());
        RUVIA_CHECK(!output.complete());
        RUVIA_CHECK(output.publishStep().status == Output::Status::kFailed);

        std::string wire;
        collectOne(buffer, wire, ruvia_ctx);
        RUVIA_CHECK_EQ(wire.size(), published.bytesAccepted);
        RUVIA_CHECK(upstream.allocations > upstream.returns);
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
}

RUVIA_TEST(http3BufferedResponseOutputExplicitStopNeverClaimsCompletion) {
    Watchdog watchdog;
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        buffer buffer(1, 1, 1, worker.resource());
        ruvia::HttpResponse response;
        response.body("not published");
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto output = makeOutput(response, plan, worker, buffer);

        output.stop();
        RUVIA_CHECK(output.failed());
        RUVIA_CHECK(!output.complete());
        const auto stopped = output.publishStep();
        RUVIA_CHECK(stopped.status == Output::Status::kFailed);
        RUVIA_CHECK(stopped.error == Output::Error::kStopped);
        RUVIA_CHECK_EQ(stopped.publishedWireBytes, 0U);
        buffer::borrowed_block block;
        Control control;
        RUVIA_CHECK(!buffer.try_receive(block));
        RUVIA_CHECK(!buffer.try_receive_control(control));
    }
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
}

}  // namespace
