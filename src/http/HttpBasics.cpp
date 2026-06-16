#include "ruvia/http/HttpTypes.h"

#include "HttpParserSyntax.h"

namespace ruvia {

bool isValidHttpHeaderName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const auto value : name) {
        if (!detail::isHttpTokenChar(static_cast<unsigned char>(value))) {
            return false;
        }
    }
    return true;
}

bool isValidHttpHeaderValue(std::string_view value) noexcept {
    for (const auto c : value) {
        if (!detail::isHttpFieldValueChar(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool isValidHttpStatusText(std::string_view value) noexcept {
    return isValidHttpHeaderValue(value);
}

HttpMethod parseMethod(std::string_view method) {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return HttpMethod::kGet;
            }
            if (method == "PUT") {
                return HttpMethod::kPut;
            }
            break;
        case 4:
            if (method == "POST") {
                return HttpMethod::kPost;
            }
            if (method == "HEAD") {
                return HttpMethod::kHead;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return HttpMethod::kPatch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return HttpMethod::kDelete;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return HttpMethod::kOptions;
            }
            break;
        default:
            break;
    }
    return HttpMethod::kUnknown;
}

std::string_view methodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet:
            return "GET";
        case HttpMethod::kPost:
            return "POST";
        case HttpMethod::kPut:
            return "PUT";
        case HttpMethod::kDelete:
            return "DELETE";
        case HttpMethod::kPatch:
            return "PATCH";
        case HttpMethod::kHead:
            return "HEAD";
        case HttpMethod::kOptions:
            return "OPTIONS";
        case HttpMethod::kUnknown:
        default:
            return "UNKNOWN";
    }
}

}  // namespace ruvia
