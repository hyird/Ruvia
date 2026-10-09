#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/ui.h>

#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace ruvia::test {

inline std::filesystem::path write_encrypted_key(tls_identity& files,
    std::string_view filename, std::string_view password) {
    const auto directory = std::filesystem::canonical(files.ca_file_.parent_path());
    if (directory.parent_path() != std::filesystem::canonical(std::filesystem::temp_directory_path())) {
        throw std::runtime_error("TLS test directory is outside its temporary root");
    }
    const auto path = directory / filename;
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(path.string().c_str(), "wb"), BIO_free);
    if (!output || password.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        ruvia::test::write_tls_private_key(output.get(), SSL_CTX_get0_privatekey(files.context_.native_handle()), password, true) != 1) {
        throw std::runtime_error("cannot write encrypted TLS test key");
    }
    return path;
}

// Restore after the behavior checks so Asio can reclaim its callback allocation
// even when an identity loader loses or replaces the caller's password userdata.
struct password_callback_cleanup final {
    explicit password_callback_cleanup(SSL_CTX* value)
        : context_(value),
          callback_(SSL_CTX_get_default_passwd_cb(value)),
          userdata_(SSL_CTX_get_default_passwd_cb_userdata(value)) {}

    ~password_callback_cleanup() {
        SSL_CTX_set_default_passwd_cb(context_, callback_);
        SSL_CTX_set_default_passwd_cb_userdata(context_, userdata_);
    }

    password_callback_cleanup(const password_callback_cleanup&) = delete;
    password_callback_cleanup& operator=(const password_callback_cleanup&) = delete;
    password_callback_cleanup(password_callback_cleanup&&) = delete;
    password_callback_cleanup& operator=(password_callback_cleanup&&) = delete;

    SSL_CTX* context_;
    pem_password_cb* callback_;
    void* userdata_;
};

class noninteractive_ui_scope final {
public:
    explicit noninteractive_ui_scope(int& attempts)
        : previous_(UI_get_default_method()),
          method_(UI_create_method("TLS test UI"), UI_destroy_method),
          attempts_(&attempts) {
        if (!method_ ||
            UI_method_set_opener(method_.get(), [](UI*) { return 1; }) != 0 ||
            UI_method_set_writer(method_.get(), [](UI*, UI_STRING*) { return 1; }) != 0 ||
            UI_method_set_closer(method_.get(), [](UI*) { return 1; }) != 0 ||
            UI_method_set_reader(method_.get(), &reject_prompt) != 0 ||
            UI_method_set_ex_data(method_.get(), 0, this) != 1) {
            throw std::runtime_error("cannot configure noninteractive TLS test UI");
        }
        UI_set_default_method(method_.get());
    }

    ~noninteractive_ui_scope() {
        UI_set_default_method(previous_);
    }

    noninteractive_ui_scope(const noninteractive_ui_scope&) = delete;
    noninteractive_ui_scope& operator=(const noninteractive_ui_scope&) = delete;
    noninteractive_ui_scope(noninteractive_ui_scope&&) = delete;
    noninteractive_ui_scope& operator=(noninteractive_ui_scope&&) = delete;

private:
    static int reject_prompt(UI* ui, UI_STRING*) noexcept {
        const auto* scope = static_cast<const noninteractive_ui_scope*>(
            UI_method_get_ex_data(UI_get_method(ui), 0));
        ++*scope->attempts_;
        return -1;
    }

    const UI_METHOD* previous_;
    std::unique_ptr<UI_METHOD, decltype(&UI_destroy_method)> method_;
    int* attempts_;
};

}  // namespace ruvia::test
