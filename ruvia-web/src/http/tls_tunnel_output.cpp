#include "http/tls_tunnel_output.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <asio.hpp>
#include <asio/ssl/error.hpp>
#include <openssl/err.h>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"

namespace ruvia::detail {

const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> tls_tunnel_output::bio_method_ = [] {
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Ruvia TLS tunnel output"), &BIO_meth_free);
    if (!method || BIO_meth_set_create(method.get(), &create_bio) != 1 ||
        BIO_meth_set_destroy(method.get(), &destroy_bio) != 1 ||
        BIO_meth_set_write_ex(method.get(), &write_bio) != 1 ||
        BIO_meth_set_ctrl(method.get(), &control_bio) != 1) {
        throw std::bad_alloc();
    }
    return method;
}();

tls_tunnel_output::tls_tunnel_output(SSL& ssl, asio::ip::tcp::socket& socket,
    const worker_handle& worker_value, std::pmr::memory_resource& resource)
    : ssl_(ssl),
      socket_(socket),
      worker_(worker_value),
      bytes_(&resource),
      available_(worker_value),
      flushed_(worker_value),
      tasks_(worker_value, {.resource_ = &resource}) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!SSL_is_init_finished(&ssl_)) {
        throw std::logic_error("TLS tunnel output requires an established TLS connection");
    }
    // Application writes are flushed in 4 KiB pieces. Space also covers TLS
    // control output generated from one buffered input record while a write waits.
    bytes_.resize(64 * 1024);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(bio_method_.get()), &BIO_free);
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

tls_tunnel_output::~tls_tunnel_output() {
    if (!worker_.is_current() || tasks_.size() != 0) {
        std::terminate();
    }
    SSL_set0_wbio(&ssl_, previous_);
}

void tls_tunnel_output::start() {
    if (!worker_.is_current() || started_) {
        std::terminate();
    }
    tasks_.spawn(run_writer());
    started_ = true;
}

int tls_tunnel_output::create_bio(BIO* bio) noexcept {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}
int tls_tunnel_output::destroy_bio(BIO* bio) noexcept {
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}
int tls_tunnel_output::write_bio(BIO* bio, const char* bytes_value, std::size_t size, std::size_t* written) noexcept {
    *written = 0;
    auto& output = *static_cast<tls_tunnel_output*>(BIO_get_data(bio));
    if (!output.worker_.is_current()) {
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
    std::memcpy(output.bytes_.data() + tail, bytes_value, first);
    std::memcpy(output.bytes_.data(), bytes_value + first, count - first);
    output.size_ += count;
    output.available_.notify();
    *written = size;
    return 1;
}
long tls_tunnel_output::control_bio(BIO*, int command, long, void*) noexcept {
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}
void tls_tunnel_output::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error;
    }
    close_socket(socket_);
    available_.notify();
    flushed_.notify();
}
void tls_tunnel_output::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    fail(std::make_error_code(std::errc::operation_canceled));
}
task<void> tls_tunnel_output::run_writer() {
    try {
        co_await run_writer_inner();
    } catch (...) {
        fail(std::make_error_code(std::errc::io_error));
        throw;
    }
}
task<void> tls_tunnel_output::run_writer_inner() {
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
        const auto result_value = co_await async_asio<std::size_t>([this, count](auto handler) {
            asio::async_write(socket_, asio::buffer(bytes_.data() + head_, count), std::move(handler));
        });
        if (result_value.error_code()) {
            fail(result_value.error_code());
            co_return;
        }
        head_ = (head_ + result_value.result()) % bytes_.size();
        size_ -= result_value.result();
        flushed_.notify();
    }
}
task<std::error_code> tls_tunnel_output::flush() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    while (!error_ && size_ != 0) {
        co_await flushed_.wait();
    }
    co_return error_;
}
task<std::error_code> tls_tunnel_output::finish() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    if (error_) {
        co_return error_;
    }
    if (!send_ended_) {
        ERR_clear_error();
        const auto result_value = SSL_shutdown(&ssl_);
        if (result_value < 0) {
            const auto code = ERR_get_error();
            fail(code == 0 ? std::make_error_code(std::errc::protocol_error)
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category()));
            co_return error_;
        }
        send_ended_ = true;
    }
    co_return co_await flush();
}
task<void> tls_tunnel_output::join() {
    stopping_ = true;
    available_.notify();
    co_await tasks_.join();
}

}  // namespace ruvia::detail
