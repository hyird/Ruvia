Task<void> HttpServer::handleSession(TcpSocket socket) {
    try {
        // Destroyed after connectionCount below, i.e. once this session has
        // left the count, so a shutdown waiting on the grace period can
        // force-close the moment the last session finishes.
        SessionDrainGuard drainNotify{this};
        ConnectionCountGuard connectionCount(activeConnectionCount_);
        std::pmr::string remoteAddress(memory_.allocator<char>());
        std::error_code remoteEc;
        const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
        if (!remoteEc) {
            assignRemoteAddress(remoteAddress, remoteEndpoint.address());
        }
        const ContextServices baseServices(
            &databases_,
            &redis_,
            &rateLimiter_,
            options_.maxBufferedBodyBytes);
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
                const auto tlsServices = baseServices.withTlsTransport(
                    remoteAddress,
                    clientCertificate);
                if (isHttp2AlpnSelected(tlsStream)) {
                    co_await handleHttp2Session(
                        tlsStream,
                        socket,
                        tlsServices);
                } else {
                    co_await handleStreamSession(
                        tlsStream,
                        socket,
                        tlsServices);
                }
            }
            closeSocket(socket);
            co_return;
        }
        co_await handleStreamSession(
            socket,
            socket,
            baseServices.withPlainTransport(remoteAddress));
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
Task<void> HttpServer::handleHttp2Session(
    Stream& stream,
    TcpSocket& socket,
    ContextServices services,
    std::string_view initialBytes) {
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);

    co_await runHttp2ServerSession(
        stream,
        socket,
        memory_,
        routes_,
        options_,
        scannerEntry,
        services,
        workerRunning_,
        initialBytes);
}
