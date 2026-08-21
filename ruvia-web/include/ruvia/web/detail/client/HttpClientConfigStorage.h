#pragma once

#include <memory_resource>

#include "ruvia/web/HttpClientHandle.h"

namespace ruvia::detail {

struct HttpClientConfigStorage final {
    HttpClientConfigStorage(const HttpClientConfig& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          scheme(source.scheme), port(source.port.value_or(source.scheme == HttpScheme::kHttps ? 443 : 80)), connectionsPerWorker(source.connectionsPerWorker), maxConcurrentHttp2StreamsPerConnection(source.maxConcurrentHttp2StreamsPerConnection), maxBufferedRequestsPerWorker(source.maxBufferedRequestsPerWorker),
          maxCookiesPerWorker(source.maxCookiesPerWorker), maxCookieBytesPerWorker(source.maxCookieBytesPerWorker),
          connectTimeout(source.connectTimeout), writeTimeout(source.writeTimeout), requestTimeout(source.requestTimeout), acquireTimeout(source.acquireTimeout),
          maxResponseBytes(source.maxResponseBytes), protocol(source.protocol), tlsPeerVerification(source.tlsPeerVerification),
          tcpNoDelay(source.tcpNoDelay), tcpKeepAlive(source.tcpKeepAlive), receivedCookies(source.receivedCookies), caFile(source.caFile, resource),
          certificateChainFile(source.certificateChainFile, resource), privateKeyFile(source.privateKeyFile, resource),
          privateKeyPassword(source.privateKeyPassword, resource), userAgent(source.userAgent, resource), cookies(resource) {
        cookies.reserve(source.cookies.size());
        for (const auto& [name, value] : source.cookies) cookies.emplace_back(std::pmr::string(name, resource), std::pmr::string(value, resource));
    }

    HttpClientConfigStorage(const HttpClientConfigStorage& source, std::pmr::memory_resource* resource)
        : host(source.host, resource), scheme(source.scheme), port(source.port), connectionsPerWorker(source.connectionsPerWorker), maxConcurrentHttp2StreamsPerConnection(source.maxConcurrentHttp2StreamsPerConnection), maxBufferedRequestsPerWorker(source.maxBufferedRequestsPerWorker),
          maxCookiesPerWorker(source.maxCookiesPerWorker), maxCookieBytesPerWorker(source.maxCookieBytesPerWorker),
          connectTimeout(source.connectTimeout), writeTimeout(source.writeTimeout), requestTimeout(source.requestTimeout), acquireTimeout(source.acquireTimeout),
          maxResponseBytes(source.maxResponseBytes), protocol(source.protocol), tlsPeerVerification(source.tlsPeerVerification),
          tcpNoDelay(source.tcpNoDelay), tcpKeepAlive(source.tcpKeepAlive), receivedCookies(source.receivedCookies), caFile(source.caFile, resource),
          certificateChainFile(source.certificateChainFile, resource), privateKeyFile(source.privateKeyFile, resource),
          privateKeyPassword(source.privateKeyPassword, resource), userAgent(source.userAgent, resource), cookies(resource) {
        cookies.reserve(source.cookies.size());
        for (const auto& [name, value] : source.cookies) cookies.emplace_back(std::pmr::string(name, resource), std::pmr::string(value, resource));
    }

    std::pmr::string host;
    HttpScheme scheme;
    std::uint16_t port;
    std::size_t connectionsPerWorker;
    std::size_t maxConcurrentHttp2StreamsPerConnection;
    std::size_t maxBufferedRequestsPerWorker;
    std::size_t maxCookiesPerWorker;
    std::size_t maxCookieBytesPerWorker;
    std::chrono::milliseconds connectTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
    std::optional<std::chrono::milliseconds> requestTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    std::size_t maxResponseBytes;
    HttpClientProtocol protocol;
    HttpClientTlsPeerVerificationPolicy tlsPeerVerification;
    TcpNoDelayPolicy tcpNoDelay;
    TcpKeepAlivePolicy tcpKeepAlive;
    HttpClientReceivedCookiePolicy receivedCookies;
    std::pmr::string caFile;
    std::pmr::string certificateChainFile;
    std::pmr::string privateKeyFile;
    std::pmr::string privateKeyPassword;
    std::pmr::string userAgent;
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> cookies;
};

struct HttpClientDefinition final {
    std::pmr::string alias;
    HttpClientConfigStorage config;
};

}  // namespace ruvia::detail
