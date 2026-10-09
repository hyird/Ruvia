#include <array>
#include <string>
#include <variant>

#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3RequestWriter.h"

#include "test_harness.h"

RUVIA_TEST(http3_request_trailers_normalize_validate_and_use_connection_qpack) {
    const std::array fields{ruvia::Http3FieldSectionFieldView{"X-Checksum", "hash"}};
    for (const bool dynamic : {false, true}) {
        ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 256, .maxBlockedStreams = 1});
        ruvia::Http3QpackDecoder decoder({.maxTableCapacity = 256, .maxBlockedStreams = 1});
        const auto bytes = dynamic ? ruvia::encodeHttp3RequestTrailers(encoder, 0, fields) : ruvia::encodeHttp3RequestTrailers(fields);
        RUVIA_CHECK((bytes.index() == 0));
        if ((bytes.index() != 0)) {
            continue;
        }
        RUVIA_CHECK((decoder.consumeEncoder(encoder.pendingEncoderOutput())).index() == 0);
        std::string name, value;
        struct Fields {
            std::string& name;
            std::string& value;
        } capture{name, value};
        const auto result = decoder.decode(0, std::get<0>(bytes), [](void* opaque, ruvia::Http3FieldSectionFieldView field) {
            auto& captured=*static_cast<Fields*>(opaque);captured.name=field.name;captured.value=field.value;return true; }, &capture);
        RUVIA_CHECK((result.index() == 0) && std::get<0>(result).status == ruvia::Http3QpackDecodeStatus::kDecoded);
        RUVIA_CHECK_EQ(name, std::string("x-checksum"));
        RUVIA_CHECK_EQ(value, std::string("hash"));
    }
    for (const auto name : {":status", "Host", "Content-Length", "Authorization", "Trailer", "Connection"}) {
        const std::array invalid{ruvia::Http3FieldSectionFieldView{name, "value"}};
        RUVIA_CHECK((ruvia::encodeHttp3RequestTrailers(invalid).index() != 0));
    }
    RUVIA_CHECK((ruvia::encodeHttp3RequestTrailers(fields, {.maxEncodedBytes = 1}).index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp3RequestTrailers(fields, {.maxDecodedBytes = 1}).index() != 0));
    RUVIA_CHECK((ruvia::encodeHttp3RequestTrailers(fields, {.maxFields = 0}).index() != 0));
    ruvia::Http3DataWritePlan plan(ruvia::Http3ClientRequestBodyPlan{.expectedLength = 3});
    const auto chunk = plan.planChunk(std::span("abc", 3), false);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK((plan.commitPayload(3, false)).index() == 0);
    RUVIA_CHECK(plan.finAllowed());
    const auto finishing = plan.planChunk({}, true);
    RUVIA_CHECK((finishing.index() == 0) && !std::get<0>(finishing).emitsData);
    RUVIA_CHECK((plan.commitPayload(0, true)).index() == 0);
    RUVIA_CHECK((plan.planChunk({}, false).index() != 0));
}
