#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

// Public stream response planning rejects a head that disagrees with its status.
RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::created);
    auto plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http_response_stream_head(
            std::move(response), ruvia::http_response_stream_kind::generic, std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
