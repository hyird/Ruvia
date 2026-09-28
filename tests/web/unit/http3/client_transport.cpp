#include <stdexcept>
#include <string>

#include <openssl/ssl.h>

#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"

#include "test_harness.h"

namespace {

ruvia::detail::Http3QuicDatagramAddress clientAddress() {
    ruvia::detail::Http3QuicDatagramAddress result;
    result.address[0] = 127;
    result.address[3] = 1;
    result.port = 4433;
    return result;
}

}  // namespace

RUVIA_TEST(http3QuicClientTransportPreservesPeerCriticalStreamAdmission) {
    using Transport = ruvia::detail::Http3QuicClientTransport;
    RUVIA_CHECK(Transport::canOpenRequestStream(0));
    RUVIA_CHECK(Transport::canOpenRequestStream(Transport::kMaxStreamsPerConnection -
                                                Transport::kPeerCriticalStreamReserve - 1));
    RUVIA_CHECK(!Transport::canOpenRequestStream(Transport::kMaxStreamsPerConnection -
                                                 Transport::kPeerCriticalStreamReserve));
    RUVIA_CHECK(!Transport::canOpenRequestStream(Transport::kMaxStreamsPerConnection));
    RUVIA_CHECK(Transport::canOpenRequestStream(0, Transport::kMaxLifetimeRequests - 1));
    RUVIA_CHECK(!Transport::canOpenRequestStream(0, Transport::kMaxLifetimeRequests));
}

RUVIA_TEST(http3QuicClientTransportInitializesAndReleasesRepeatedly) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    for (int iteration = 0; iteration != 2; ++iteration) {
        Http3QuicDatagramBridge bridge(clientAddress());
        {
            Http3QuicClientTransport transport(tls, bridge, clientAddress(), "example.test");
            RUVIA_CHECK(transport.connectionInfo() == Http3QuicClientTransport::State::kNotStarted);
            RUVIA_CHECK(!transport.eventTimeout());
            RUVIA_CHECK(transport.handleEvents() == Http3QuicClientTransport::State::kNotStarted);
            RUVIA_CHECK(transport.openLocalBidirectionalStream().error ==
                        Http3QuicStreamSet::Error::kHandshakePending);
            RUVIA_CHECK(transport.openLocalUnidirectionalStream().error ==
                        Http3QuicStreamSet::Error::kHandshakePending);
            RUVIA_CHECK(transport.acceptPeerStreams().error ==
                        Http3QuicStreamSet::Error::kHandshakePending);
            const auto notStarted = transport.terminateRequestStream(0);
            RUVIA_CHECK(notStarted.send == Http3QuicStreamSet::Error::kHandshakePending);
            RUVIA_CHECK(notStarted.close == Http3QuicStreamSet::Error::kHandshakePending);
            transport.close();
            RUVIA_CHECK(transport.terminateRequestStream(0).close ==
                        Http3QuicStreamSet::Error::kClosed);
            RUVIA_CHECK(transport.openLocalBidirectionalStream().error ==
                        Http3QuicStreamSet::Error::kClosed);
        }
    }
#endif
}

RUVIA_TEST(http3QuicClientTransportRejectsMissingBioAndInvalidHost) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    Http3QuicClientTlsContext tls(ClientTransportConfigView{});
    {
        Http3QuicDatagramBridge bridge(clientAddress());
        BIO_free(bridge.releaseSslBio());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicClientTransport transport(tls, bridge, clientAddress(), "example.test");
        }));
    }
    {
        Http3QuicDatagramBridge bridge(clientAddress());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicClientTransport transport(tls, bridge, clientAddress(), "bad host");
        }));
        // prepare() fails before ownership of the bridge BIO is transferred.
        BIO_free(bridge.releaseSslBio());
    }
    {
        Http3QuicDatagramBridge bridge(clientAddress());
        auto invalidPeer = clientAddress();
        invalidPeer.port = 0;
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicClientTransport transport(tls, bridge, invalidPeer, "example.test");
        }));
        BIO_free(bridge.releaseSslBio());
    }
#endif
}
