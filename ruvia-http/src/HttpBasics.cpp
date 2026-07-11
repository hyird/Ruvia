#include "ruvia/http/HttpCommon.h"

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

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
    if (!value.empty()) {
        const auto first = static_cast<unsigned char>(value.front());
        const auto last = static_cast<unsigned char>(value.back());
        if (first == ' ' || first == '\t' || last == ' ' || last == '\t') {
            return false;
        }
    }
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

HttpKnownMethod classifyHttpMethod(std::string_view method) noexcept {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return HttpKnownMethod::kGet;
            }
            if (method == "PUT") {
                return HttpKnownMethod::kPut;
            }
            break;
        case 4:
            if (method == "POST") {
                return HttpKnownMethod::kPost;
            }
            if (method == "HEAD") {
                return HttpKnownMethod::kHead;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return HttpKnownMethod::kPatch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return HttpKnownMethod::kDelete;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return HttpKnownMethod::kOptions;
            }
            if (method == "CONNECT") {
                return HttpKnownMethod::kConnect;
            }
            break;
        default:
            break;
    }
    return HttpKnownMethod::kUnknown;
}

std::string_view knownHttpMethodToken(HttpKnownMethod method) noexcept {
    switch (method) {
        case HttpKnownMethod::kGet:
            return "GET";
        case HttpKnownMethod::kPost:
            return "POST";
        case HttpKnownMethod::kPut:
            return "PUT";
        case HttpKnownMethod::kDelete:
            return "DELETE";
        case HttpKnownMethod::kPatch:
            return "PATCH";
        case HttpKnownMethod::kHead:
            return "HEAD";
        case HttpKnownMethod::kOptions:
            return "OPTIONS";
        case HttpKnownMethod::kConnect:
            return "CONNECT";
        case HttpKnownMethod::kUnknown:
        default:
            return {};
    }
}

bool isValidHttpMethodToken(std::string_view method) noexcept {
    if (method.empty()) {
        return false;
    }
    for (const auto ch : method) {
        if (!detail::isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia
