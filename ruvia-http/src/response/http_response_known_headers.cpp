#include "ruvia/http/detail/response/http_response_known_headers.h"

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] unsigned char lower_ascii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

}  // namespace

std::uint32_t classify_response_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    const auto first = lower_ascii(static_cast<unsigned char>(name.front()));
    switch (name.size()) {
        case 4:
            switch (first) {
                case 'd':
                    if (http_ascii_equals_ignore_case(name, "Date")) {
                        return response_header_date;
                    }
                    break;
                case 'e':
                    if (http_ascii_equals_ignore_case(name, "ETag")) {
                        return response_header_etag;
                    }
                    break;
                case 'v':
                    if (http_ascii_equals_ignore_case(name, "Vary")) {
                        return response_header_vary;
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case 5:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Allow")) {
                return response_header_allow;
            }
            return 0;
        case 6:
            if (first == 's' && http_ascii_equals_ignore_case(name, "Server")) {
                return response_header_server;
            }
            return 0;
        case 8:
            if (first == 'l' && http_ascii_equals_ignore_case(name, "Location")) {
                return response_header_location;
            }
            return 0;
        case 10:
            switch (first) {
                case 'c':
                    if (http_ascii_equals_ignore_case(name, "Connection")) {
                        return response_header_connection;
                    }
                    break;
                case 's':
                    if (http_ascii_equals_ignore_case(name, "Set-Cookie")) {
                        return response_header_set_cookie;
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case 12:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Type")) {
                return response_header_content_type;
            }
            return 0;
        case 13:
            switch (first) {
                case 'a':
                    if (http_ascii_equals_ignore_case(name, "Accept-Ranges")) {
                        return response_header_accept_ranges;
                    }
                    break;
                case 'c':
                    if (http_ascii_equals_ignore_case(name, "Cache-Control")) {
                        return response_header_cache_control;
                    }
                    if (http_ascii_equals_ignore_case(name, "Content-Range")) {
                        return response_header_content_range;
                    }
                    break;
                case 'l':
                    if (http_ascii_equals_ignore_case(name, "Last-Modified")) {
                        return response_header_last_modified;
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case 14:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Length")) {
                return response_header_content_length;
            }
            return 0;
        case 16:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Encoding")) {
                return response_header_content_encoding;
            }
            return 0;
        case 17:
            if (first == 't' && http_ascii_equals_ignore_case(name, "Transfer-Encoding")) {
                return response_header_transfer_encoding;
            }
            return 0;
        case 27:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin")) {
                return response_header_access_control_allow_origin;
            }
            return 0;
        case 28:
            if (first == 'a') {
                if (http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods")) {
                    return response_header_access_control_allow_methods;
                }
                if (http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers")) {
                    return response_header_access_control_allow_headers;
                }
            }
            return 0;
        case 22:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Access-Control-Max-Age")) {
                return response_header_access_control_max_age;
            }
            return 0;
        case 29:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers")) {
                return response_header_access_control_expose_headers;
            }
            return 0;
        case 32:
            if (first == 'a' &&
                http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials")) {
                return response_header_access_control_allow_credentials;
            }
            return 0;
        default:
            return 0;
    }
}

}  // namespace ruvia::detail
