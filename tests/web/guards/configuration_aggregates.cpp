#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <type_traits>

#include <ruvia/core/OperationOptions.h>
#include <ruvia/http/Http2Connection.h>
#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpStatus.h>
#include <ruvia/http/Sse.h>
#include <ruvia/web/App.h>
#include <ruvia/web/ContextRequest.h>
#include <ruvia/web/ErrorHandlers.h>
#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/RateLimitRule.h>
#include <ruvia/web/SecurityHeaders.h>
#include <ruvia/web/ServerConfig.h>
#include <ruvia/web/ValidationIssue.h>
#include <ruvia/web/WebSocket.h>
#ifdef RUVIA_ENABLE_JWT
#include <ruvia/web/auth/Jwt.h>
#endif
#include <ruvia/web/db/DbTypes.h>
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/RedisTypes.h>
#endif

namespace {

template <typename Value>
void consume(Value&&) {}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    static_assert(std::is_aggregate_v<ruvia::OperationOptions>);
    consume(ruvia::OperationOptions{.timeout = 1ms});
    consume(ruvia::OperationOptions{.stopToken = {}});

    static_assert(std::is_aggregate_v<ruvia::HttpOriginOptions>);
    consume(ruvia::HttpOriginOptions{.host = "localhost"});
    consume(ruvia::HttpOriginOptions{.port = 80});
    consume(ruvia::Http2RegularRequestHeadView{.method = "POST"});
    consume(ruvia::Http2RegularRequestHeadView{.content = ruvia::Http2RequestContent::none()});
    consume(ruvia::Http2ConnectRequestHeadView{.authority = "example.test:443"});
    consume(ruvia::Http2ConnectRequestHeadView{.headers = {}});
    consume(ruvia::Http2ExtendedConnectRequestHeadView{.protocol = "websocket"});
    consume(ruvia::Http2ExtendedConnectRequestHeadView{.headers = {}});
    consume(ruvia::SseMessage{.data = ruvia::BorrowedText{"payload"}});
    consume(ruvia::SseMessage{.retry = 1ms});

    static_assert(std::is_aggregate_v<ruvia::ServerConfig>);
    consume(ruvia::ServerConfig{.workerCount = 1});
    consume(ruvia::ServerConfig{.memoryPool = {}});
    consume(ruvia::TlsClientCertificateConfig{.verifyFile = std::nullopt});
    consume(ruvia::TlsClientCertificateConfig{.requirement = ruvia::TlsClientCertificateRequirement::kOptional});
    consume(ruvia::TlsSniConfig{.host = "example.test"});
    consume(ruvia::TlsSniConfig{.privateKeyPassword = {}});
    consume(ruvia::TlsConfig{.certificateChainFile = {}});
    consume(ruvia::TlsConfig{.sni = {}});
    consume(ruvia::ListenConfig{.address = "127.0.0.1"});
    consume(ruvia::ListenConfig{.autoHttpsRedirect = false});
    consume(ruvia::CorsOriginConfig{.mode = ruvia::CorsOriginMode::kAny});
    consume(ruvia::CorsOriginConfig{.value = {}});
    consume(ruvia::CorsRequestHeadersConfig{.mode = ruvia::CorsRequestHeadersMode::kReflect});
    consume(ruvia::CorsRequestHeadersConfig{.names = {}});
    consume(ruvia::CorsConfig{.origin = {}});
    consume(ruvia::CorsConfig{.maxAge = std::nullopt});
    consume(ruvia::DocumentRootConfig{.root = {}});
    consume(ruvia::DocumentRootConfig{.precompressMaxBytes = 1});

    consume(ruvia::HttpClientRegistrationConfig{.alias = "default"});
    consume(ruvia::HttpClientRegistrationConfig{.config = {}});
#ifdef RUVIA_ENABLE_DATABASE
    consume(ruvia::DbRegistrationConfig{.alias = "default"});
    consume(ruvia::DbRegistrationConfig{.config = {}});
#endif
#ifdef RUVIA_ENABLE_REDIS
    consume(ruvia::RedisRegistrationConfig{.alias = "default"});
    consume(ruvia::RedisRegistrationConfig{.config = {}});
#endif

    consume(ruvia::SignedCookieLookupOptions{.name = "cookie"});
    consume(ruvia::SignedCookieLookupOptions{.secret = "secret"});
    consume(ruvia::ScopedErrorHandlerOptions{.prefix = "/api"});
    consume(ruvia::ScopedErrorHandlerOptions{.handler = nullptr});
    consume(ruvia::ScopedNotFoundHandlerOptions{.prefix = "/api"});
    consume(ruvia::ScopedNotFoundHandlerOptions{.handler = nullptr});

    static_assert(std::is_aggregate_v<ruvia::HttpClientConfig>);
    consume(ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps});
    consume(ruvia::HttpClientConfig{.cookies = {}});
    consume(ruvia::RateLimitConfig{.rule = {}});
    consume(ruvia::RateLimitConfig{.capacityPerWorker = 1});
    consume(ruvia::SecurityHeader{.name = "X-Test"});
    consume(ruvia::SecurityHeader{.value = "enabled"});
    consume(ruvia::SecurityHeadersConfig{.contentTypeOptionsHeader = ruvia::DefaultSecurityHeaderPolicy::kEmitDefault});
    consume(ruvia::SecurityHeadersConfig{.existingHeaders = ruvia::SecurityHeaderConflictPolicy::kPreserveExisting});
    consume(ruvia::ValidationIssueOptions{.field = "field"});
    consume(ruvia::ValidationIssueOptions{.resource = std::pmr::get_default_resource()});
    consume(ruvia::WebSocketRouteConfig{.subprotocols = {}});
    consume(ruvia::WebSocketRouteConfig{.lifecycle = {}});

#ifdef RUVIA_ENABLE_JWT
    consume(ruvia::JwtClaimOptions{.name = "claim"});
    consume(ruvia::JwtClaimOptions{.value = "value"});
    consume(ruvia::JwtSignOptions{.algorithm = ruvia::JwtAlgorithm::kHs256});
    consume(ruvia::JwtSignOptions{.resource = std::pmr::get_default_resource()});
    consume(ruvia::JwtVerifyOptions{.token = "token"});
    consume(ruvia::JwtVerifyOptions{.resource = std::pmr::get_default_resource()});
    consume(ruvia::JwtDecodeUnverifiedOptions{.token = "token"});
    consume(ruvia::JwtDecodeUnverifiedOptions{.resource = std::pmr::get_default_resource()});
#endif

    consume(ruvia::DbConfig{.driver = ruvia::DbDriver::kUnspecified});
    consume(ruvia::DbConfig{.acquireTimeout = std::nullopt});
#ifdef RUVIA_ENABLE_REDIS
    consume(ruvia::RedisConfig{.host = "127.0.0.1"});
    consume(ruvia::RedisConfig{.tcpKeepAlive = ruvia::TcpKeepAlivePolicy::kSystemDefault});
    consume(ruvia::RedisStreamReadView{.stream = "events"});
    consume(ruvia::RedisStreamReadView{.id = ">"});
    consume(ruvia::RedisXReadGroupOptions{.count = std::nullopt});
    consume(ruvia::RedisXReadGroupOptions{.acknowledgement = ruvia::RedisXReadGroupAcknowledgementPolicy::kTrackPending});
    consume(ruvia::RedisSetOptions{.condition = std::nullopt});
    consume(ruvia::RedisSetOptions{.previousValue = ruvia::RedisSetPreviousValuePolicy::kDiscard});
    consume(ruvia::RedisScanOptions{.cursor = std::nullopt});
    consume(ruvia::RedisScanOptions{.count = std::nullopt});
#endif
}
