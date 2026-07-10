#include "test_harness.h"

#include <cstddef>
#include <limits>

#include "ruvia/http/detail/http2/Http2StreamBodyAccounting.h"

namespace {

using ruvia::detail::Http2StreamBodyAccounting;

}  // namespace

RUVIA_TEST(body_accounting_content_length_conflict) {
    Http2StreamBodyAccounting accounting;
    RUVIA_CHECK(!accounting.hasContentLength());
    RUVIA_CHECK(accounting.setContentLength(100));
    RUVIA_CHECK(accounting.hasContentLength());
    RUVIA_CHECK_EQ(accounting.contentLength(), std::size_t{100});
    RUVIA_CHECK(accounting.setContentLength(100));    // repeating the same value is fine
    RUVIA_CHECK(!accounting.setContentLength(200));   // a conflicting Content-Length is rejected
}

RUVIA_TEST(body_accounting_received_bytes_overflow_safe) {
    Http2StreamBodyAccounting accounting;
    RUVIA_CHECK(accounting.addReceivedBytes(50));
    RUVIA_CHECK(accounting.addReceivedBytes(50));
    RUVIA_CHECK_EQ(accounting.receivedBytes(), std::size_t{100});
    // Accumulating past SIZE_MAX is refused rather than wrapping.
    accounting.setReceivedBytes(std::numeric_limits<std::size_t>::max() - 5);
    RUVIA_CHECK(!accounting.addReceivedBytes(10));
    RUVIA_CHECK(accounting.addReceivedBytes(5));  // exactly reaches SIZE_MAX
}

RUVIA_TEST(body_accounting_length_completeness) {
    Http2StreamBodyAccounting noLength;
    // With no declared Content-Length a body is always "complete" and never "exceeds".
    RUVIA_CHECK(noLength.lengthComplete());
    RUVIA_CHECK(!noLength.exceedsContentLength());
    RUVIA_CHECK(noLength.addReceivedBytes(1000));
    RUVIA_CHECK(noLength.lengthComplete());

    Http2StreamBodyAccounting declared;
    RUVIA_CHECK(declared.setContentLength(100));
    RUVIA_CHECK(declared.addReceivedBytes(50));
    RUVIA_CHECK(!declared.lengthComplete());        // short of the declared length
    RUVIA_CHECK(!declared.exceedsContentLength());
    RUVIA_CHECK(declared.addReceivedBytes(50));
    RUVIA_CHECK(declared.lengthComplete());          // exactly the declared length
    RUVIA_CHECK(declared.addReceivedBytes(1));
    RUVIA_CHECK(declared.exceedsContentLength());     // more than declared (RFC 7540 8.1.2.6)
    RUVIA_CHECK(!declared.lengthComplete());
}
