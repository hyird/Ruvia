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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/multipart_reader.h"

#include "body/http_request_body_facade.h"
#include "context/context_access.h"
#include "context_request_fixture.h"
#include "context_services_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::multipart_reader;
using ruvia::task;

struct chunk_source final {
    std::vector<std::string> chunks_;
    std::size_t index_{};
    bool fail_{};

    task<std::optional<std::span<const std::byte>>> read() {
        if (index_ == chunks_.size() && fail_) {
            throw std::runtime_error("injected body read failure");
        }
        if (index_ == chunks_.size()) {
            co_return std::nullopt;
        }
        co_return ruvia::as_bytes(chunks_[index_++]);
    }
};

struct cancelled_chunk_source final {
    explicit cancelled_chunk_source(const ruvia::worker_handle& worker_value)
        : signal_(worker_value),
          chunk_(std::string("--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-cancelled-field-xxxxxxxxxxxxxxxx\"\r\n\r\n") + std::string(4096, 'x')) {}

    task<std::optional<std::span<const std::byte>>> read() {
        if (!supplied_) {
            supplied_ = true;
            co_return ruvia::as_bytes(chunk_);
        }
        waiting_ = true;
        co_await signal_.wait();
        throw std::system_error(asio::error::operation_aborted);
    }

    ruvia::worker_signal signal_;
    std::string chunk_;
    bool supplied_{false};
    bool waiting_{false};
};

task<void> read_one(multipart_reader& reader_value) {
    (void)co_await reader_value.read();
}

task<void> drain(multipart_reader& reader_value, std::size_t& parts, std::string* saved_name = nullptr,
    std::string* saved_body = nullptr) {
    while (auto part = co_await reader_value.read()) {
        if (parts == 0 && saved_name != nullptr) {
            saved_name->assign(part->name());
            saved_body->assign(part->body());
        }
        if (part->phase() == ruvia::multipart_chunk_phase::first ||
            part->phase() == ruvia::multipart_chunk_phase::complete) {
            ++parts;
        }
    }
}

void run(task<void> task_value) {
    asio::io_context io;
    auto result_value = asio::co_spawn(io, ruvia::as_awaitable(std::move(task_value)), asio::use_future);
    io.run();
    result_value.get();
}

std::string many_parts() {
    std::string body;
    for (std::size_t i = 0; i < 80; ++i) {
        body += "--BOUNDARY\r\nContent-Disposition: form-data; name=\"field-";
        body += std::string(96, static_cast<char>('a' + i % 26));
        body += "\"\r\n\r\nvalue-" + std::to_string(i) + "\r\n";
    }
    body += "--BOUNDARY--\r\n";
    return body;
}

std::vector<std::string> fixed_chunks(std::string_view body) {
    std::vector<std::string> chunks;
    for (std::size_t offset = 0; offset < body.size(); offset += 8192) {
        chunks.emplace_back(body.substr(offset, 8192));
    }
    return chunks;
}

}  // namespace

RUVIA_TEST(multipart_reader_reuses_parser_memory_across_many_parts) {
    ruvia::test::counting_memory_resource resource;
    chunk_source source_value{fixed_chunks(many_parts())};
    ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
    multipart_reader reader_value(binding.facade(),
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});

    std::size_t parts{};
    std::string saved_name;
    std::string saved_body;
    run(drain(reader_value, parts, &saved_name, &saved_body));
    RUVIA_CHECK_EQ(parts, std::size_t{80});
    RUVIA_CHECK(saved_name.starts_with("field-"));
    RUVIA_CHECK_EQ(saved_body, std::string("value-0"));
    RUVIA_CHECK(resource.allocation_count() > 0);
    RUVIA_CHECK(resource.allocation_count() < 80);
    RUVIA_CHECK(resource.deallocation_count() > 0);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(multipart_reader_dropped_cold_read_keeps_reader_usable) {
    ruvia::test::counting_memory_resource resource;
    chunk_source source_value{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"field-long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue\r\n--BOUNDARY--\r\n"}};
    ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
    multipart_reader reader_value(binding.facade(),
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});

    {
        auto cold = reader_value.read();
    }
    std::size_t parts{};
    run(drain(reader_value, parts));
    RUVIA_CHECK_EQ(parts, std::size_t{1});
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(multipart_reader_releases_parser_memory_after_parse_failure) {
    ruvia::test::counting_memory_resource resource;
    chunk_source source_value{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue"}};
    ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
    multipart_reader reader_value(binding.facade(),
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});

    bool failed{};
    std::size_t parts{};
    try {
        run(drain(reader_value, parts));
    } catch (const std::exception&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK(resource.deallocation_count() > 0);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool rejected{};
    try {
        run(read_one(reader_value));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(multipart_reader_releases_parser_memory_after_body_source_failure) {
    ruvia::test::counting_memory_resource resource;
    chunk_source source_value{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-name-xxxxxxxxxxxxxxxx\"\r\n\r\nvalue"}, 0, true};
    ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
    multipart_reader reader_value(binding.facade(),
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});

    bool failed{};
    std::size_t parts{};
    try {
        run(drain(reader_value, parts));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK(resource.deallocation_count() > 0);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool rejected{};
    try {
        run(read_one(reader_value));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(multipart_reader_releases_parser_memory_when_parent_reader_expires) {
    ruvia::test::counting_memory_resource resource;
    chunk_source source_value{{std::string("--BOUNDARY\r\nContent-Disposition: form-data; name=\"long-field-name-xxxxxxxxxxxxxxxx\"\r\n\r\n") + std::string(4096, 'x')}};
    std::optional<multipart_reader> reader;
    using read_operation_type = ruvia::scoped_operation<std::optional<ruvia::multipart_stream_part>>;
    std::unique_ptr<read_operation_type> cold;
    {
        ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
        reader.emplace(binding.facade(),
            ruvia::multipart_parse_options{.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});
        run(read_one(*reader));
        RUVIA_CHECK(resource.live_allocations() > 0);
        cold.reset(new read_operation_type(reader->read()));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool rejected{};
    try {
        (void)reader->read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    reader.reset();
    auto await_expired = [&]() -> task<void> {
        bool expired{};
        try {
            (void)co_await std::move(*cold);
        } catch (const std::logic_error&) {
            expired = true;
        }
        RUVIA_CHECK(expired);
    };
    run(await_expired());
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(multipart_reader_cooperative_cancellation_retires_parser_before_completion) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    ruvia::detail::body_reader_binding<cancelled_chunk_source> binding(worker_value);
    multipart_reader reader_value(binding.facade(),
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = &resource});
    auto exercise = [&]() -> task<void> {
        try {
            const auto first = co_await reader_value.read();
            RUVIA_CHECK(first.has_value());
            const std::string saved_name(first ? first->name() : std::string_view{});
            bool cancelled{};
            try {
                (void)co_await reader_value.read();
            } catch (const std::system_error& error) {
                cancelled = error.code() == asio::error::operation_aborted;
            }
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK(saved_name.starts_with("long-cancelled-field"));
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            bool rejected{};
            try {
                (void)co_await reader_value.read();
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
    RUVIA_CHECK(binding.reader().waiting_);
    RUVIA_CHECK(resource.live_allocations() > 0);
    RUVIA_CHECK(worker_value.post([&binding] { binding.reader().signal_.notify(); }).accepted());
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(context_multipart_reader_uses_worker_pool_not_request_arena) {
    ruvia::worker_memory worker;
    std::array<std::byte, 64 * 1024> arena_storage{};
    ruvia::request_memory memory(worker, arena_storage);
    constexpr std::string_view content_type_value = "multipart/form-data; boundary=BOUNDARY";
    const ruvia::http_header_view headers[]{ruvia::http_header_view{"Content-Type", content_type_value}};
    auto request = make_request(memory.resource(), "/", headers, "POST");
    chunk_source source_value{{"--BOUNDARY\r\nContent-Disposition: form-data; name=\"x-long-name-xxxxxxxxxxxxxxxx\"\r\n\r\ny\r\n--BOUNDARY--\r\n"}};
    ruvia::detail::body_reader_binding<chunk_source> binding(std::move(source_value));
    auto context_value = ruvia::detail::context_access::make(memory, request,
        ruvia::test::test_context_services().with_streaming_request_body(binding.facade()));

    auto* const before = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    auto reader_value = context_value.req().get_multipart_reader();
    std::size_t parts{};
    run(drain(reader_value, parts));
    auto* const after = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    RUVIA_CHECK_EQ(parts, std::size_t{1});
    RUVIA_CHECK_EQ(after - before, std::ptrdiff_t{1});
}
