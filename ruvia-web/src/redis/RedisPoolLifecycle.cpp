#include <hiredis/hiredis.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <openssl/ssl.h>

#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const WorkerHandle& requireRedisWorker(const WorkerHandle& worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("redis pool requires a valid worker");
    }
    return worker;
}

}  // namespace

void RedisReaderDeleter::operator()(redisReader* reader) const noexcept {
    if (reader != nullptr) {
        redisReaderFree(reader);
    }
}

RedisPool::Connection::Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : socket(ioContext),
      resolver(ioContext),
      writeBuffer(detail::pmrResourceOrDefault(resource)),
      reader(redisReaderCreate()),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(resource)) {}

RedisPool::Connection::~Connection() = default;

RedisPool::Connection::Connection(Connection&&) noexcept = default;
RedisPool::Connection& RedisPool::Connection::operator=(Connection&&) noexcept = default;

RedisPool::RedisPool(asio::io_context& ioContext, const RedisConfigStorage& config,
    std::optional<std::chrono::milliseconds> commandTimeout, std::size_t poolSize,
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      worker_(requireRedisWorker(worker)),
      config_(config),
      commandTimeout_(commandTimeout),
      resource_(detail::pmrResourceOrDefault(resource)),
      connections_(resource_),
      scheduler_(poolSize, worker_, resource_),
      cancellationMailbox_(makeWorkerCancellationMailbox(*this, worker_)) {
    if (config_.tls.mode == client_tls_mode::verify_identity) {
        tls_context_.emplace(asio::ssl::context::tls_client);
        tls_context_->set_verify_mode(asio::ssl::verify_peer);
        if (SSL_CTX_set_min_proto_version(tls_context_->native_handle(), TLS1_2_VERSION) != 1) {
            throw std::runtime_error("configuring Redis minimum TLS version failed");
        }
        if (config_.tls.ca_file.empty()) {
            tls_context_->set_default_verify_paths();
        } else {
            tls_context_->load_verify_file(std::string(config_.tls.ca_file));
        }
        if (!config_.tls.certificate_file.empty()) {
            tls_context_->use_certificate_chain_file(std::string(config_.tls.certificate_file));
            tls_context_->set_password_callback([](std::size_t, asio::ssl::context::password_purpose) { return std::string{}; });
            tls_context_->use_private_key_file(std::string(config_.tls.private_key_file), asio::ssl::context::pem);
            if (SSL_CTX_check_private_key(tls_context_->native_handle()) != 1) {
                throw std::invalid_argument("Redis TLS certificate does not match its private key");
            }
        }
    }
    connections_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
    }
}

RedisPool::~RedisPool() {
    closeNow();
}

Task<void> RedisPool::connect() {
    for (auto& connection : connections_) {
        if (!connection.connected) {
            co_await connect(connection);
        }
    }
    co_return;
}

void RedisPool::closeNow() noexcept {
    cancellationMailbox_->detach(*this);
    if (!scheduler_.close()) {
        return;
    }
    for (auto& connection : connections_) {
        if (connection.abortReason == Connection::AbortReason::kNone) {
            connection.abortReason = Connection::AbortReason::kClosing;
        }
        close(connection);
    }
}

}  // namespace ruvia::detail
