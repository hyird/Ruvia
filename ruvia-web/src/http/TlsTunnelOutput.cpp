#include "http/TlsTunnelOutput.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <asio.hpp>
#include <asio/ssl/error.hpp>
#include <openssl/err.h>

#include "ruvia/core/Async.h"
#include "ruvia/core/Socket.h"

namespace ruvia::detail {

const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> TlsTunnelOutput::bioMethod_ = [] {
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Ruvia TLS tunnel output"), &BIO_meth_free);
    if (!method || BIO_meth_set_create(method.get(), &createBio) != 1 ||
        BIO_meth_set_destroy(method.get(), &destroyBio) != 1 ||
        BIO_meth_set_write_ex(method.get(), &writeBio) != 1 ||
        BIO_meth_set_ctrl(method.get(), &controlBio) != 1) {
        throw std::bad_alloc();
    }
    return method;
}();

TlsTunnelOutput::TlsTunnelOutput(SSL& ssl, asio::ip::tcp::socket& socket,
    const WorkerHandle& worker, std::pmr::memory_resource& resource)
    : ssl_(ssl),
      socket_(socket),
      worker_(worker),
      bytes_(&resource),
      available_(worker),
      flushed_(worker),
      tasks_(worker, {.resource = &resource}) {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (!SSL_is_init_finished(&ssl_)) {
        throw std::logic_error("TLS tunnel output requires an established TLS connection");
    }
    // Application writes are flushed in 4 KiB pieces. Space also covers TLS
    // control output generated from one buffered input record while a write waits.
    bytes_.resize(64 * 1024);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(bioMethod_.get()), &BIO_free);
    if (!bio) {
        throw std::bad_alloc();
    }
    previous_ = SSL_get_wbio(&ssl_);
    if (previous_ == nullptr || BIO_up_ref(previous_) != 1) {
        previous_ = nullptr;
        throw std::bad_alloc();
    }
    BIO_set_data(bio.get(), this);
    SSL_set0_wbio(&ssl_, bio.release());
}

TlsTunnelOutput::~TlsTunnelOutput() {
    if (!worker_.isCurrent() || tasks_.size() != 0) {
        std::terminate();
    }
    SSL_set0_wbio(&ssl_, previous_);
}

void TlsTunnelOutput::start() {
    if (!worker_.isCurrent() || started_) {
        std::terminate();
    }
    tasks_.spawn(runWriter());
    started_ = true;
}

int TlsTunnelOutput::createBio(BIO* bio) noexcept {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}
int TlsTunnelOutput::destroyBio(BIO* bio) noexcept {
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}
int TlsTunnelOutput::writeBio(BIO* bio, const char* bytes, std::size_t size, std::size_t* written) noexcept {
    *written = 0;
    auto& output = *static_cast<TlsTunnelOutput*>(BIO_get_data(bio));
    if (!output.worker_.isCurrent()) {
        std::terminate();
    }
    BIO_clear_retry_flags(bio);
    if (size == 0) {
        return 1;
    }
    const auto count = size;
    if (output.error_ || output.stopping_ || count > output.bytes_.size() - output.size_) {
        output.fail(std::make_error_code(std::errc::no_buffer_space));
        return 0;
    }
    const auto tail = (output.head_ + output.size_) % output.bytes_.size();
    const auto first = std::min(count, output.bytes_.size() - tail);
    std::memcpy(output.bytes_.data() + tail, bytes, first);
    std::memcpy(output.bytes_.data(), bytes + first, count - first);
    output.size_ += count;
    output.available_.notify();
    *written = size;
    return 1;
}
long TlsTunnelOutput::controlBio(BIO*, int command, long, void*) noexcept {
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}
void TlsTunnelOutput::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error;
    }
    closeSocket(socket_);
    available_.notify();
    flushed_.notify();
}
void TlsTunnelOutput::abort() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    fail(std::make_error_code(std::errc::operation_canceled));
}
Task<void> TlsTunnelOutput::runWriter() {
    try {
        co_await runWriterInner();
    } catch (...) {
        fail(std::make_error_code(std::errc::io_error));
        throw;
    }
}
Task<void> TlsTunnelOutput::runWriterInner() {
    for (;;) {
        if (error_) {
            co_return;
        }
        if (size_ == 0) {
            flushed_.notify();
            if (stopping_) {
                co_return;
            }
            co_await available_.wait();
            continue;
        }
        const auto count = std::min(size_, bytes_.size() - head_);
        const auto result = co_await asyncAsio<std::size_t>([this, count](auto handler) {
            asio::async_write(socket_, asio::buffer(bytes_.data() + head_, count), std::move(handler));
        });
        if (result.errorCode()) {
            fail(result.errorCode());
            co_return;
        }
        head_ = (head_ + result.result()) % bytes_.size();
        size_ -= result.result();
        flushed_.notify();
    }
}
Task<std::error_code> TlsTunnelOutput::flush() {
    if (!worker_.isCurrent() || !started_) {
        std::terminate();
    }
    while (!error_ && size_ != 0) {
        co_await flushed_.wait();
    }
    co_return error_;
}
Task<std::error_code> TlsTunnelOutput::finish() {
    if (!worker_.isCurrent() || !started_) {
        std::terminate();
    }
    if (error_) {
        co_return error_;
    }
    if (!sendEnded_) {
        ERR_clear_error();
        const auto result = SSL_shutdown(&ssl_);
        if (result < 0) {
            const auto code = ERR_get_error();
            fail(code == 0 ? std::make_error_code(std::errc::protocol_error)
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category()));
            co_return error_;
        }
        sendEnded_ = true;
    }
    co_return co_await flush();
}
Task<void> TlsTunnelOutput::join() {
    stopping_ = true;
    available_.notify();
    co_await tasks_.join();
}

}  // namespace ruvia::detail
