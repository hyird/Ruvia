#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "http/StreamingInternal.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Streaming.h"

namespace {

struct CaptureStreamSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
    std::vector<std::string> writes;
};

ruvia::Task<void> writeChunk(void* target, std::string_view chunk) {
    static_cast<CaptureStreamSink*>(target)->writes.emplace_back(chunk);
    co_return;
}

ruvia::Task<void> endStream(void*) {
    co_return;
}

ruvia::Task<void> sleepStream(void*, std::chrono::milliseconds) {
    co_return;
}

void bindContext(void*, ruvia::Context*) noexcept {}

std::pmr::string& scratch(void* target) noexcept {
    return static_cast<CaptureStreamSink*>(target)->scratch;
}

void addTrailer(void*, std::string_view, std::string_view) {}

bool committed(void*) noexcept {
    return false;
}

bool aborted(void*) noexcept {
    return false;
}

ruvia::ResponseStreamWriter makeWriter(CaptureStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink,
        &writeChunk,
        &endStream,
        &sleepStream,
        &bindContext,
        &scratch,
        &addTrailer,
        &committed,
        &aborted);
}

ruvia::Task<void> writeLines(ruvia::ResponseStreamWriter& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

}  // namespace

RUVIA_TEST(response_stream_writeln_reuses_scratch_without_leaking_previous_chunk) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(writeLines(writer)),
        asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("first\n"));
    RUVIA_CHECK_EQ(sink.writes[1], std::string("second\n"));
}
