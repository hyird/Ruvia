Task<void> HttpServer::handleSession(TcpSocket socket) {
    try {
        ConnectionCountGuard connectionCount(activeConnectionCount_);
        if (options_.tls.enabled) {
            ConnectionScanner::Entry handshakeEntry;
            {
                ConnectionScanner::Guard handshakeGuard(&connectionScanner_, handshakeEntry, socket);
                handshakeEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
                asio::ssl::stream<TcpSocket&> tlsStream(socket, *tlsContext_);
                const auto ec = co_await asyncError(TlsServerHandshakeInitiator{&tlsStream});
                if (ec) {
                    closeSocket(socket);
                    co_return;
                }
                handshakeEntry.touch();
                std::pmr::string clientCertificate(memory_.allocator<char>());
                extractTlsClientCertificate(tlsStream.native_handle(), clientCertificate);
                if (isHttp2AlpnSelected(tlsStream)) {
                    co_await handleHttp2Session(tlsStream, socket, {}, clientCertificate);
                } else {
                    co_await handleStreamSession(tlsStream, socket, clientCertificate);
                }
            }
            closeSocket(socket);
            co_return;
        }
        co_await handleStreamSession(socket, socket);
    } catch (...) {
        // Last-resort safety net: any exception that escapes the session
        // body (including bad_alloc, error-handler failures, or framework
        // bugs) must not propagate into asio::detached, which terminates.
        // Socket state may be partially written or completely fine; we
        // cannot safely emit anything new, so just drop the connection.
        closeSocket(socket);
    }
}

template <typename Stream>
Task<void> HttpServer::handleHttp2Session(Stream& stream, TcpSocket& socket, std::string_view initialBytes, std::string_view clientCertificate) {
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);

    std::pmr::string remoteAddress(memory_.allocator<char>());
    std::error_code remoteEc;
    const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
    if (!remoteEc) {
        assignRemoteAddress(remoteAddress, remoteEndpoint.address());
    }

    co_await runHttp2ServerSession(
        stream,
        socket,
        memory_,
        routes_,
        databases_,
        redis_,
        httpClients_,
        options_,
        scannerEntry,
        remoteAddress,
        rateLimiter_,
        clientCertificate,
        initialBytes,
        &started_);
}
