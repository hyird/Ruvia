#include <array>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/Bytes.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include "context_request_fixture.h"
#include "context_services_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::MultipartReader;
using ruvia::Task;

struct ChunkSource final {
    std::vector<std::string> chunks;
    std::size_t index{};
    bool fail{};

    Task<std::optional<std::span<const std::byte>>> read() {
        if (index == chunks.size() && fail) {
            throw std::runtime_error("injected body read failure");
        }
        if (index == chunks.size()) {
            co_return std::nullopt;
        }
        co_return ruvia::asBytes(chunks[index++]);
    }
};

struct CancelledChunkSource final {
    explicit CancelledChunkSource(const ruvia::WorkerHandle& worker)
        : signal(worker),
          chunk(std::string("--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-cancelled-field-xxxxxxxxxxxxxxxx\"\r\n\r\n") + std::string(4096, 'x')) {}

    Task<std::optional<std::span<const std::byte>>> read() {
        if (!supplied) {
            supplied = true;
            co_return ruvia::asBytes(chunk);
        }
        waiting = true;
        co_await signal.wait();
        throw std::system_error(asio::error::operation_aborted);
    }

    ruvia::WorkerSignal signal;
    std::string chunk;
    bool supplied{false};
    bool waiting{false};
};

Task<void> readOne(MultipartReader& reader) {
    (void)co_await reader.read();
}

Task<void> drain(MultipartReader& reader, std::size_t& parts, std::string* savedName = nullptr,
    std::string* savedBody = nullptr) {
    while (auto part = co_await reader.read()) {
        if (parts == 0 && savedName != nullptr) {
            savedName->assign(part->name());
            savedBody->assign(part->body());
        }
        if (part->phase() == ruvia::MultipartChunkPhase::kFirst ||
            part->phase() == ruvia::MultipartChunkPhase::kComplete) {
            ++parts;
        }
    }
}

void run(Task<void> task) {
    asio::io_context io;
    auto result = asio::co_spawn(io, ruvia::asAwaitable(std::move(task)), asio::use_future);
    io.run();
    result.get();
}

std::string manyParts() {
    std::string body;
    for (std::size_t i = 0; i < 80; ++i) {
        body += "--BOUNDARY\r\nContent-Disposition: form-data; name=\"field-";
        body += std::string(96, static_cast<char>('a' + i % 26));
        body += "\"\r\n\r\nvalue-" + std::to_string(i) + "\r\n";
    }
    body += "--BOUNDARY--\r\n";
    return body;
}

std::vector<std::string> fixedChunks(std::string_view body) {
    std::vector<std::string> chunks;
    for (std::size_t offset = 0; offset < body.size(); offset += 8192) {
        chunks.emplace_back(body.substr(offset, 8192));
    }
    return chunks;
}

}  // namespace

RUVIA_TEST(multipart_reader_reuses_parser_memory_across_many_parts) {
    ruvia::test::CountingMemoryResource resource;
    ChunkSource source{fixedChunks(manyParts())};
    ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
    MultipartReader reader(binding.facade(),
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});

    std::size_t parts{};
    std::string savedName;
    std::string savedBody;
    run(drain(reader, parts, &savedName, &savedBody));
    RUVIA_CHECK_EQ(parts, std::size_t{80});
    RUVIA_CHECK(savedName.starts_with("field-"));
    RUVIA_CHECK_EQ(savedBody, std::string("value-0"));
    RUVIA_CHECK(resource.allocationCount() > 0);
    RUVIA_CHECK(resource.allocationCount() < 80);
    RUVIA_CHECK(resource.deallocationCount() > 0);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(multipart_reader_dropped_cold_read_keeps_reader_usable) {
    ruvia::test::CountingMemoryResource resource;
    ChunkSource source{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"field-long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue\r\n--BOUNDARY--\r\n"}};
    ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
    MultipartReader reader(binding.facade(),
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});

    {
        auto cold = reader.read();
    }
    std::size_t parts{};
    run(drain(reader, parts));
    RUVIA_CHECK_EQ(parts, std::size_t{1});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(multipart_reader_releases_parser_memory_after_parse_failure) {
    ruvia::test::CountingMemoryResource resource;
    ChunkSource source{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue"}};
    ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
    MultipartReader reader(binding.facade(),
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});

    bool failed{};
    std::size_t parts{};
    try {
        run(drain(reader, parts));
    } catch (const std::exception&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK(resource.deallocationCount() > 0);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    bool rejected{};
    try {
        run(readOne(reader));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(multipart_reader_releases_parser_memory_after_body_source_failure) {
    ruvia::test::CountingMemoryResource resource;
    ChunkSource source{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue"}, 0, true};
    ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
    MultipartReader reader(binding.facade(),
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});

    bool failed{};
    std::size_t parts{};
    try {
        run(drain(reader, parts));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK(resource.deallocationCount() > 0);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    bool rejected{};
    try {
        run(readOne(reader));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(multipart_reader_releases_parser_memory_when_parent_reader_expires) {
    ruvia::test::CountingMemoryResource resource;
    ChunkSource source{{std::string("--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-field-name-xxxxxxxxxxxxxxxx\"\r\n\r\n") + std::string(4096, 'x')}};
    std::optional<MultipartReader> reader;
    using ReadOperation = ruvia::ScopedOperation<std::optional<ruvia::MultipartStreamPart>>;
    std::unique_ptr<ReadOperation> cold;
    {
        ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
        reader.emplace(binding.facade(),
            ruvia::MultipartParseOptions{.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});
        run(readOne(*reader));
        RUVIA_CHECK(resource.liveAllocations() > 0);
        cold.reset(new ReadOperation(reader->read()));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    bool rejected{};
    try {
        (void)reader->read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    reader.reset();
    auto awaitExpired = [&]() -> Task<void> {
        bool expired{};
        try {
            (void)co_await std::move(*cold);
        } catch (const std::logic_error&) {
            expired = true;
        }
        RUVIA_CHECK(expired);
    };
    run(awaitExpired());
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(multipart_reader_cooperative_cancellation_retires_parser_before_completion) {
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::BodyReaderBinding<CancelledChunkSource> binding(worker);
    MultipartReader reader(binding.facade(),
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = &resource});
    auto exercise = [&]() -> Task<void> {
        try {
            const auto first = co_await reader.read();
            RUVIA_CHECK(first.has_value());
            const std::string savedName(first ? first->name() : std::string_view{});
            bool cancelled{};
            try {
                (void)co_await reader.read();
            } catch (const std::system_error& error) {
                cancelled = error.code() == asio::error::operation_aborted;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK(savedName.starts_with("long-cancelled-field"));
            RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
            bool rejected{};
            try {
                (void)co_await reader.read();
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        } catch (...) {
            attachment.stop();
            throw;
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(exercise());
    io.poll();
    RUVIA_CHECK(binding.reader().waiting);
    RUVIA_CHECK(resource.liveAllocations() > 0);
    RUVIA_CHECK(worker.post([&binding] { binding.reader().signal.notify(); }).accepted());
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(context_multipart_reader_uses_worker_pool_not_request_arena) {
    ruvia::WorkerMemory worker;
    std::array<std::byte, 64 * 1024> arenaStorage{};
    ruvia::RequestMemory memory(worker, arenaStorage);
    constexpr std::string_view contentType = "multipart/form-data; boundary=BOUNDARY";
    const ruvia::HttpHeaderView headers[]{ruvia::HttpHeaderView{"Content-Type", contentType}};
    auto request = makeRequest(memory.resource(), "/", headers, "POST");
    ChunkSource source{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"x-long-name-xxxxxxxxxxxxxxxx\"\r\n\r\ny\r\n--BOUNDARY--\r\n"}};
    ruvia::detail::BodyReaderBinding<ChunkSource> binding(std::move(source));
    auto context = ruvia::detail::ContextAccess::make(memory, request,
        ruvia::test::testContextServices().withStreamingRequestBody(binding.facade()));

    auto* const before = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    auto reader = context.req().multipartReader();
    std::size_t parts{};
    run(drain(reader, parts));
    auto* const after = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    RUVIA_CHECK_EQ(parts, std::size_t{1});
    RUVIA_CHECK_EQ(after - before, std::ptrdiff_t{1});
}
