#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

#include <openssl/pem.h>
#include <openssl/ssl.h>

namespace ruvia::detail {

// Borrow the configured password only during synchronous credential loading.
// An absent password uses a caller provider, or an empty noninteractive default.
class tls_password_scope final {
public:
    tls_password_scope(SSL_CTX& context_value, std::string_view password) noexcept
        : context_(&context_value),
          password_(password),
          previous_callback_(SSL_CTX_get_default_passwd_cb(&context_value)),
          previous_userdata_(SSL_CTX_get_default_passwd_cb_userdata(&context_value)) {
        const bool default_source = previous_userdata_ == nullptr &&
                                    (previous_callback_ == nullptr || previous_callback_ == PEM_def_callback);
        if (!password.empty() || default_source) {
            SSL_CTX_set_default_passwd_cb(context_, &copy_password);
            SSL_CTX_set_default_passwd_cb_userdata(context_, this);
        }
    }

    ~tls_password_scope() {
        // The caller may own its userdata: Asio deletes its callback object.
        SSL_CTX_set_default_passwd_cb(context_, previous_callback_);
        SSL_CTX_set_default_passwd_cb_userdata(context_, previous_userdata_);
    }

    tls_password_scope(const tls_password_scope&) = delete;
    tls_password_scope& operator=(const tls_password_scope&) = delete;
    tls_password_scope(tls_password_scope&&) = delete;
    tls_password_scope& operator=(tls_password_scope&&) = delete;

private:
    static int copy_password(char* buffer, int size, int, void* userdata) noexcept {
        if (buffer == nullptr || size <= 0 || userdata == nullptr) {
            return -1;
        }
        const auto password = static_cast<const tls_password_scope*>(userdata)->password_;
        if (password.size() > static_cast<std::size_t>(size)) {
            return -1;
        }
        if (!password.empty()) {
            std::memcpy(buffer, password.data(), password.size());
        }
        return static_cast<int>(password.size());
    }

    SSL_CTX* context_;
    std::string_view password_;
    pem_password_cb* previous_callback_;
    void* previous_userdata_;
};

}  // namespace ruvia::detail
