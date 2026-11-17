#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http1_chunked_body.h"
#include "http1_connection_plan.h"
#include "http1_message_body.h"
#include "http2_connection.h"
#include "http_cookie_jar.h"
#include "http_multipart.h"
#include "http_range.h"
#include "http_response.h"
#include "http_server_sent_events.h"
#include "proxy_protocol.h"

// Replacement operators belong only to this isolated test executable.
// The thread-local arm confines each fault to one public operation.
namespace {
std::atomic<std::size_t> live_allocations{};
thread_local bool counting = false;
thread_local std::size_t allocation_index = 0;
thread_local std::size_t failure_index = (std::numeric_limits<std::size_t>::max)();

void before_allocation() {
    if (counting && allocation_index++ == failure_index) {
        throw std::bad_alloc();
    }
}

void release_allocation(void* pointer) noexcept {
    if (pointer != nullptr) {
        std::free(pointer);
        --live_allocations;
    }
}

void release_aligned_allocation(void* pointer) noexcept {
    if (pointer != nullptr) {
        void* storage = nullptr;
        std::memcpy(&storage, static_cast<char*>(pointer) - sizeof(storage), sizeof(storage));
        std::free(storage);
        --live_allocations;
    }
}

struct allocation_scope final {
    explicit allocation_scope(std::size_t failure) {
        allocation_index = 0;
        failure_index = failure;
        counting = true;
    }
    ~allocation_scope() {
        counting = false;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct outbound_allocation_observation final {
    std::size_t allocations{};
    bool failed{};
    bool terminal{};
};
}  // namespace

void* operator new(std::size_t bytes) {
    before_allocation();
    void* pointer = std::malloc(bytes == 0 ? 1 : bytes);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    ++live_allocations;
    return pointer;
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    before_allocation();
    const auto requested_alignment = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    bytes = std::max(bytes, std::size_t{1});
    const auto overhead = sizeof(void*) + requested_alignment - 1;
    if (bytes > (std::numeric_limits<std::size_t>::max)() - overhead) {
        throw std::bad_alloc();
    }
    void* storage = std::malloc(bytes + overhead);
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    void* pointer = static_cast<char*>(storage) + sizeof(storage);
    auto space = bytes + requested_alignment - 1;
    if (std::align(requested_alignment, bytes, pointer, space) == nullptr) {
        std::terminate();
    }
    std::memcpy(static_cast<char*>(pointer) - sizeof(storage), &storage, sizeof(storage));
    ++live_allocations;
    return pointer;
}
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* pointer) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    release_aligned_allocation(pointer);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    release_allocation(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    release_allocation(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned_allocation(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned_allocation(pointer);
}

outbound_allocation_observation exercise_warmed_outbound_compression(std::size_t failure) {
    const auto initial_live = live_allocations.load();
    outbound_allocation_observation observed;
    {
        std::deque<std::string> retained;
        auto client = ruvia::http2_connection::client({});
        auto server = ruvia::http2_connection::server({});
        const auto transfer = [&](ruvia::http2_connection& from, ruvia::http2_connection& to) {
            auto& bytes = retained.emplace_back();
            (void)from.take_output_batch((std::numeric_limits<std::size_t>::max)(), bytes);
            require(to.feed(bytes) == ruvia::http2_feed_result::accepted, "peer rejected encoded output");
        };
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        for (unsigned round = 0; round != 2; ++round) {
            const auto submitted = client.submit_request_head(ruvia::http_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/fault"});
            require(std::holds_alternative<std::uint32_t>(submitted), "request rejected");
            transfer(client, server);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            require(head != nullptr, "missing request head");
            auto end = server.next_event();
            require(end && end->message_end() != nullptr, "missing request end");
            const std::string value(round == 0 ? 16 : 2000, round == 0 ? 'a' : 'b');
            ruvia::http_response response;
            response.header("x-fault", value);
            response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
            const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
            bool committed = false;
            if (round == 1) {
                allocation_scope scope(failure);
                try {
                    const auto result = server.submit_response_head(head->stream_id(), response, plan);
                    committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(result);
                } catch (const std::bad_alloc&) {
                    observed.failed = true;
                }
                observed.allocations = allocation_index;
            } else {
                const auto result = server.submit_response_head(head->stream_id(), response, plan);
                committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(result);
            }
            observed.terminal = server.connection_error().has_value();
            if (observed.failed && observed.terminal) {
                const auto retry = server.submit_response_head(head->stream_id(), response, plan);
                const auto* error = std::get_if<ruvia::http_response_head_error>(&retry);
                require(error != nullptr && *error == ruvia::http_response_head_error::closed, "fatal compression failure accepted another submission");
            } else {
                if (observed.failed) {
                    const auto retry = server.submit_response_head(head->stream_id(), response, plan);
                    committed = std::holds_alternative<ruvia::http_buffered_response_write_plan>(retry);
                }
                require(committed, "recoverable preparation failure could not commit");
                transfer(server, client);
                auto received = client.next_event();
                auto* received_head = received ? received->response_head() : nullptr;
                require(received_head != nullptr, "peer lost response header after compression");
                bool matched = false;
                for (const auto& field : received_head->head().headers()) {
                    if (field.name() == "x-fault") {
                        matched = field.value() == value;
                    }
                }
                require(matched, "peer decoded a stale compressed field");
                auto received_end = client.next_event();
                require(received_end && received_end->message_end() != nullptr, "peer lost response end");
                require(!client.connection_error(), "peer compression state was corrupted");
            }
            (void)server.release(std::move(*head));
        }
    }
    require(live_allocations.load() == initial_live, "connection or encoder leaked an allocation");
    return observed;
}

TEST(Ruvia, http2_outbound_hpack_allocation_failures_preserve_state_and_reclaim_memory) {
    const auto baseline = exercise_warmed_outbound_compression((std::numeric_limits<std::size_t>::max)());
    ASSERT_FALSE(baseline.failed);
    ASSERT_FALSE(baseline.terminal);
    ASSERT_NE(baseline.allocations, 0U);
    std::size_t terminal = 0;
    for (std::size_t index = 0; index != baseline.allocations; ++index) {
        const auto observed = exercise_warmed_outbound_compression(index);
        EXPECT_TRUE(observed.failed) << "allocation " << index;
        terminal += observed.terminal;
    }
    EXPECT_NE(terminal, 0U);
    ::testing::Test::RecordProperty("allocation_failure_sites", std::to_string(baseline.allocations));
    ::testing::Test::RecordProperty("terminal_failure_sites", std::to_string(terminal));
}

TEST(Ruvia, multipart_range_slicer_can_retry_after_an_allocation_failure) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 6);
    ruvia::http_byte_range_slicer slicer(ranges, 6, "text/plain", "parts");
    std::string output;
    slicer.write("a", output);
    ASSERT_EQ(output, "--parts\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-1/6\r\n\r\na");
    output.reserve(output.size() + 1);
    // The first payload byte fits, but appending the following CRLF must grow.
    output.resize(output.capacity() - 1, '_');
    const auto before = output;
    bool failed = false;
    {
        allocation_scope scope(0);
        try {
            slicer.write("bcdef", output);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
    }
    ASSERT_TRUE(failed);
    EXPECT_EQ(output, before);
    ASSERT_EQ(slicer.current_offset(), 1U);
    EXPECT_FALSE(slicer.complete());

    slicer.write("bcdef", output);
    EXPECT_TRUE(slicer.complete());
    EXPECT_EQ(output, before + "b\r\n--parts\r\nContent-Type: text/plain\r\nContent-Range: bytes 4-5/6\r\n\r\nef\r\n--parts--\r\n");
}

TEST(Ruvia, sse_parser_releases_large_discarded_field_storage_before_the_next_event) {
    auto ignored_id = std::string("id: ") + std::string(128 * 1024, 'x');
    ignored_id.append("\0\n", 2);
    const auto replaced_type = std::string("event: ") + std::string(128 * 1024, 'x') + "\nevent: small\n";

    for (const std::string_view input : {std::string_view(ignored_id), std::string_view(replaced_type)}) {
        ruvia::sse_parser parser;
        const auto before = live_allocations.load();
        const auto result = parser.parse(input);
        const auto after = live_allocations.load();
        EXPECT_EQ(after, before);
        EXPECT_TRUE(result.holds<ruvia::decode_need_input>());
        const auto dispatched = parser.parse("data: ok\n\n");
        const auto* event = dispatched.get_if<ruvia::sse_event>();
        ASSERT_NE(event, nullptr);
        EXPECT_EQ(event->data(), "ok");
        EXPECT_TRUE(event->last_event_id().empty());
    }
}

TEST(Ruvia, sse_parser_transfers_large_ids_and_discards_unpublished_updates) {
    const std::string large_id(128 * 1024, 'x');
    const auto field = "id: " + large_id + '\n';
    const auto oversized_data = "data: " + large_id + "x\n\n";
    ruvia::sse_parser parser({.event_limit_ = ruvia::protocol_byte_limit::limited(large_id.size())});
    const auto empty = live_allocations.load();

    ASSERT_TRUE(parser.parse(field).holds<ruvia::decode_need_input>());
    EXPECT_TRUE(parser.last_event_id().empty());
    const auto pending = live_allocations.load();
    ASSERT_GT(pending, empty);

    const auto dispatched = parser.parse("data: ok\n\n");
    const auto published = live_allocations.load();
    const auto* event = dispatched.get_if<ruvia::sse_event>();
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->last_event_id(), large_id);
    EXPECT_EQ(published, pending);

    ASSERT_TRUE(parser.parse("id:\n\n").holds<ruvia::decode_need_input>());
    EXPECT_TRUE(parser.last_event_id().empty());
    EXPECT_EQ(live_allocations.load(), empty);

    ASSERT_TRUE(parser.parse("id: previous\n\n").holds<ruvia::decode_need_input>());
    ASSERT_TRUE(parser.parse(field).holds<ruvia::decode_need_input>());
    parser.finish();
    EXPECT_EQ(parser.last_event_id(), "previous");
    EXPECT_EQ(live_allocations.load(), empty);

    parser.reset();
    ASSERT_TRUE(parser.parse("id: previous\n\n").holds<ruvia::decode_need_input>());
    ASSERT_TRUE(parser.parse(field).holds<ruvia::decode_need_input>());
    const auto failed = parser.parse(oversized_data);
    EXPECT_TRUE(failed.holds<ruvia::sse_parse_failure>());
    EXPECT_EQ(parser.last_event_id(), "previous");
    EXPECT_EQ(live_allocations.load(), empty);
}

TEST(Ruvia, multipart_parser_releases_oversized_buffers_at_terminal_poll_and_reset) {
    const auto boundary = ruvia::parse_multipart_boundary("multipart/form-data; boundary=b");
    ruvia::multipart_parser parser({.boundary_ = std::get<ruvia::multipart_boundary>(boundary)});
    const std::string payload(128 * 1024, 'x');
    const auto body = "--b\r\nContent-Disposition: form-data; name=field\r\n\r\n" + payload + "\r\n--b--\r\n";
    const auto empty_body = "--b--\r\n" + payload;
    const auto empty = live_allocations.load();

    parser.feed(body);
    const auto result = parser.poll();
    const auto* part = result.get_if<ruvia::multipart_stream_part>();
    ASSERT_NE(part, nullptr);
    EXPECT_EQ(part->name(), "field");
    EXPECT_EQ(part->body(), payload);
    ASSERT_TRUE(parser.poll().holds<ruvia::decode_complete>());
    const auto finished = live_allocations.load();
    EXPECT_EQ(finished, empty);
    EXPECT_TRUE(parser.poll().holds<ruvia::decode_complete>());

    parser.reset();
    parser.feed(payload);
    const auto rejected = parser.poll();
    const auto* failure = rejected.get_if<ruvia::multipart_parse_failure>();
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error(), ruvia::multipart_parse_error::preamble_too_large);
    const auto failed = live_allocations.load();
    EXPECT_EQ(failed, empty);
    const auto repeated = parser.poll();
    ASSERT_TRUE(repeated.holds<ruvia::multipart_parse_failure>());
    EXPECT_EQ(repeated.get_if<ruvia::multipart_parse_failure>()->error(), failure->error());

    parser.reset();
    parser.feed(body);
    parser.reset();
    const auto reset = live_allocations.load();
    EXPECT_EQ(reset, empty);
    parser.feed(empty_body);
    EXPECT_TRUE(parser.poll().holds<ruvia::decode_complete>());
    EXPECT_EQ(live_allocations.load(), empty);
}

TEST(Ruvia, cookie_jar_releases_storage_when_a_large_value_is_replaced) {
    ruvia::http_cookie_jar jar(ruvia::http_scheme::https, "example.test", {.max_cookie_bytes_ = 4096});
    const auto now = std::chrono::system_clock::time_point{};
    const auto large_cookie = "id=" + std::string(4000, 'x');
    const auto shorter_cookie = "id=" + std::string(256, 'y');

    ASSERT_TRUE(jar.absorb_set_cookie("/", "id=a", now));
    const auto small = live_allocations.load();
    ASSERT_TRUE(jar.absorb_set_cookie("/", large_cookie, now));
    const auto large = live_allocations.load();
    ASSERT_GT(large, small);
    EXPECT_THROW(
        {
            allocation_scope scope(0);
            (void)jar.absorb_set_cookie("/", shorter_cookie, now);
        },
        std::bad_alloc);
    EXPECT_EQ(jar.cookie_header("/", true, now), large_cookie);
    ASSERT_TRUE(jar.absorb_set_cookie("/", "id=b", now));
    const auto replaced = live_allocations.load();

    EXPECT_EQ(jar.cookie_header("/", true, now), "id=b");
    EXPECT_EQ(replaced, small);
}

TEST(Ruvia, chunked_decoder_retries_trailers_from_the_same_input_after_allocation_failure) {
    constexpr std::string_view ending = "\r\n0;ext\r\nX-Trace: retained\r\n\r\n";
    ruvia::http1_chunked_body_decoder decoder;
    const auto body = decoder.decode("1\r\nx\r");
    ASSERT_TRUE(body.holds<ruvia::decode_output>());
    ASSERT_EQ(body.consumed_bytes(), 4U);

    EXPECT_THROW(
        {
            allocation_scope scope(0);
            (void)decoder.decode(ending);
        },
        std::bad_alloc);
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::active);

    const auto retried = decoder.decode(ending);
    const auto* complete = retried.get_if<ruvia::http1_chunk_decode_complete>();
    ASSERT_NE(complete, nullptr);
    EXPECT_EQ(retried.consumed_bytes(), ending.size());
    ASSERT_EQ(complete->trailers().size(), 1U);
    EXPECT_EQ(complete->trailers().front().name(), "x-trace");
    EXPECT_EQ(complete->trailers().front().value(), "retained");
}

TEST(Ruvia, proxy_parser_reclaims_large_scratch_after_its_views_expire) {
    using namespace std::string_view_literals;
    constexpr auto tlv = "\xE0\x00\x10ghijklmnopqrstuv"sv;
    constexpr auto tlv_count = 512U;
    constexpr auto block_size = 12U + tlv_count * tlv.size();
    auto wire = std::string("\r\n\r\n\0\r\nQUIT\n\x21\x11", 14);
    wire.push_back(static_cast<char>(block_size >> 8));
    wire.push_back(static_cast<char>(block_size & 0xffU));
    wire.append(12, '\0');
    for (std::size_t i = 0; i < tlv_count; ++i) {
        wire.append(tlv);
    }
    const auto bytes = std::as_bytes(std::span<const char>(wire.data(), wire.size()));
    auto local = wire;
    local[12] = '\x20';
    local[13] = '\0';

    ruvia::proxy_protocol_parser parser({.max_v2_header_bytes_ = wire.size()});
    const auto empty = live_allocations.load();
    ASSERT_TRUE(parser.parse(bytes).holds<ruvia::decode_complete>());
    const auto tlvs = parser.header().tlvs();
    ASSERT_EQ(tlvs.size(), tlv_count);
    EXPECT_EQ(tlvs.back().value_.size(), 16U);
    EXPECT_EQ(tlvs.back().value_.back(), std::byte{'v'});
    parser.reset();
    EXPECT_EQ(live_allocations.load(), empty);

    const auto local_bytes = std::as_bytes(std::span<const char>(local.data(), local.size()));
    constexpr std::size_t fixed_header_size = 16;
    ASSERT_TRUE(parser.parse(local_bytes.first(fixed_header_size)).holds<ruvia::decode_need_input>());
    const auto local_result = [&] {
        allocation_scope scope(0);
        return parser.parse(local_bytes.subspan(fixed_header_size));
    }();
    ASSERT_TRUE(local_result.holds<ruvia::decode_complete>());
    EXPECT_EQ(local_result.consumed_bytes(), local.size() - fixed_header_size);
    EXPECT_TRUE(parser.header().tlvs().empty());

    parser.reset();
    ASSERT_TRUE(parser.parse(bytes.first(bytes.size() - 1)).holds<ruvia::decode_need_input>());
    EXPECT_THROW(
        {
            allocation_scope scope(0);
            (void)parser.parse(bytes.last(1));
        },
        std::bad_alloc);
    EXPECT_EQ(parser.state(), ruvia::decoder_state::failed);
    EXPECT_EQ(live_allocations.load(), empty);
}

TEST(Ruvia, chunked_decoder_reclaims_oversized_trailer_storage) {
    constexpr auto field_count = ruvia::max_http_header_fields;
    std::string wire = "0\r\n";
    for (std::size_t index = 0; index < field_count; ++index) {
        wire.append("X-Trace: ok\r\n");
    }
    const auto malformed = wire + "invalid-trailer\r\n\r\n";
    wire.append("\r\n");

    for (const bool reset : {false, true}) {
        ruvia::http1_chunked_body_decoder decoder;
        const auto baseline = live_allocations.load();
        const auto result = decoder.decode(wire);
        const auto* complete = result.get_if<ruvia::http1_chunk_decode_complete>();
        ASSERT_NE(complete, nullptr);
        ASSERT_EQ(complete->trailers().size(), field_count);
        EXPECT_EQ(complete->trailers().back().value(), "ok");
        if (reset) {
            decoder.reset();
            EXPECT_EQ(decoder.state(), ruvia::decoder_state::active);
        } else {
            const auto replayed = decoder.decode("NEXT");
            ASSERT_TRUE(replayed.holds<ruvia::http1_chunk_decode_complete>());
            EXPECT_EQ(replayed.consumed_bytes(), 0U);
            EXPECT_TRUE(replayed.get_if<ruvia::http1_chunk_decode_complete>()->trailers().empty());
        }
        EXPECT_EQ(live_allocations.load(), baseline);
    }

    ruvia::http1_chunked_body_decoder decoder;
    const auto baseline = live_allocations.load();
    const auto rejected = decoder.decode(malformed);
    ASSERT_TRUE(rejected.holds<ruvia::http1_chunk_decode_failure>());
    EXPECT_EQ(rejected.get_if<ruvia::http1_chunk_decode_failure>()->error(), ruvia::http1_chunk_decode_error::invalid_framing);
    EXPECT_EQ(live_allocations.load(), baseline);
}

TEST(Ruvia, http1_body_decoder_releases_trailers_before_finishing) {
    std::string wire = "0\r\n";
    for (std::size_t i = 0; i < ruvia::max_http_header_fields; ++i) {
        wire.append("X-Trace: ok\r\n");
    }
    wire.append("\r\n");
    ruvia::http1_body_decoder decoder(ruvia::http1_message_framing::chunked());
    const auto baseline = live_allocations.load();

    const auto result = decoder.decode(wire, {});
    const auto* trailers = result.get_if<ruvia::http1_body_trailers>();
    ASSERT_NE(trailers, nullptr);
    ASSERT_EQ(trailers->fields().size(), ruvia::max_http_header_fields);
    EXPECT_EQ(trailers->fields().back().value(), "ok");

    const auto closed = decoder.finish_input("NEXT", {});
    const auto remaining = live_allocations.load();
    EXPECT_TRUE(closed.holds<ruvia::decode_complete>());
    EXPECT_EQ(closed.consumed_bytes(), 0U);
    EXPECT_EQ(remaining, baseline);
}

TEST(Ruvia, http1_body_decoder_preserves_completed_body_if_codec_recreation_fails) {
    ruvia::http_content_codings codings;
    codings.push_back(ruvia::http_content_coding::gzip);
    ruvia::http1_body_decoder decoder(ruvia::http1_message_framing::chunked(codings));
    decoder.discard_content();
    ASSERT_TRUE(decoder.decode("0\r\n\r\n", {}).holds<ruvia::http1_body_trailers>());
    ASSERT_TRUE(decoder.decode({}, {}).holds<ruvia::decode_complete>());

    const auto baseline = live_allocations.load();
    for (const auto failure_index_value : {0U, 1U}) {
        EXPECT_THROW({
            allocation_scope scope(failure_index_value);
            decoder.reset();
        },
            std::bad_alloc);
        EXPECT_EQ(live_allocations.load(), baseline);
        ASSERT_EQ(decoder.state(), ruvia::decoder_state::finished);
        const auto replayed = decoder.decode("NEXT", {});
        EXPECT_TRUE(replayed.holds<ruvia::decode_complete>());
        EXPECT_EQ(replayed.consumed_bytes(), 0U);
    }

    decoder.reset();
    std::array<char, 64> scratch{};
    const auto rejected = decoder.decode("3\r\nbad\r\n0\r\n\r\n", scratch);
    const auto* failure = rejected.get_if<ruvia::http1_body_decode_failure>();
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error(), ruvia::http1_body_decode_error::invalid_transfer_coding);
}
