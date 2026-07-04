#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <utility>

#include "net/http2/Http2SessionResults.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::Http2DataWindowResult;
using ruvia::detail::Http2InputReadResult;
using ruvia::detail::Http2RouteDispatchResult;
using ruvia::detail::Http2SessionFlow;

// The single-flag result types are constexpr; guard their state->shouldStop
// mapping at compile time.
static_assert(!Http2SessionFlow::keepRunning().shouldStop());
static_assert(Http2SessionFlow::stopRunning().shouldStop());
static_assert(!Http2InputReadResult::ready().shouldStop());
static_assert(Http2InputReadResult::stopReading().shouldStop());
static_assert(!Http2DataWindowResult::ready().shouldStop());
static_assert(Http2DataWindowResult::stopWriting().shouldStop());

}  // namespace

RUVIA_TEST(session_flow_flags_map_state_to_should_stop) {
    RUVIA_CHECK(!Http2SessionFlow::keepRunning().shouldStop());
    RUVIA_CHECK(Http2SessionFlow::stopRunning().shouldStop());
    RUVIA_CHECK(!Http2InputReadResult::ready().shouldStop());
    RUVIA_CHECK(Http2InputReadResult::stopReading().shouldStop());
    RUVIA_CHECK(!Http2DataWindowResult::ready().shouldStop());
    RUVIA_CHECK(Http2DataWindowResult::stopWriting().shouldStop());
}

RUVIA_TEST(route_dispatch_stream_handled) {
    const auto result = Http2RouteDispatchResult::makeStreamHandled();
    RUVIA_CHECK(result.streamHandled());
    RUVIA_CHECK(!result.bufferedResponse());
}

RUVIA_TEST(route_dispatch_buffered_response_carries_response) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(201);
    auto result = Http2RouteDispatchResult::makeBufferedResponse(std::move(response));
    RUVIA_CHECK(!result.streamHandled());
    RUVIA_CHECK(result.bufferedResponse());
    // takeResponse moves out the carried response intact.
    const auto taken = result.takeResponse();
    RUVIA_CHECK_EQ(taken.status(), std::uint16_t{201});
}
