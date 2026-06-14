#include "ruvia/auth/Jwt.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ruvia {
namespace {

constexpr std::string_view kBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] bool isReservedClaim(std::string_view name) noexcept {
    return name == "iss" || name == "sub" || name == "aud" || name == "exp" ||
        name == "nbf" || name == "iat" || name == "jti";
}

[[nodiscard]] std::pmr::memory_resource* resourceOrDefault(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

[[nodiscard]] std::pmr::string makePmrString(std::string_view value, std::pmr::memory_resource* resource) {
    return std::pmr::string(value.data(), value.size(), resourceOrDefault(resource));
}

void appendJsonEscaped(std::pmr::string& out, std::string_view value) {
    out.push_back('"');
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out.append("\\u00");
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::string_view value) {
    if (!first) { out.push_back(','); }
    first = false;
    appendJsonEscaped(out, name);
    out.push_back(':');
    appendJsonEscaped(out, value);
}

void appendJsonMember(std::pmr::string& out, bool& first, std::string_view name, std::int64_t value) {
    if (!first) { out.push_back(','); }
    first = false;
    appendJsonEscaped(out, name);
    out.push_back(':');
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) { throw std::logic_error("failed to format JWT numeric claim"); }
    out.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

[[nodiscard]] std::pmr::string base64UrlEncode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(resourceOrDefault(resource));
    out.reserve((input.size() * 4 + 2) / 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kBase64Url[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kBase64Url[(buffer << (6 - bits)) & 0x3F]);
    }
    return out;
}

[[nodiscard]] int decodeBase64UrlChar(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') { return ch - 'A'; }
    if (ch >= 'a' && ch <= 'z') { return ch - 'a' + 26; }
    if (ch >= '0' && ch <= '9') { return ch - '0' + 52; }
    if (ch == '-') { return 62; }
    if (ch == '_') { return 63; }
    return -1;
}

[[nodiscard]] std::pmr::string base64UrlDecode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(resourceOrDefault(resource));
    out.reserve(input.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        const auto value = decodeBase64UrlChar(ch);
        if (value < 0) { throw std::invalid_argument("JWT base64url value is invalid"); }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

[[nodiscard]] const EVP_MD* digestFor(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256: return EVP_sha256();
        case JwtAlgorithm::kHs384: return EVP_sha384();
        case JwtAlgorithm::kHs512: return EVP_sha512();
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

[[nodiscard]] std::string_view algorithmName(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256: return "HS256";
        case JwtAlgorithm::kHs384: return "HS384";
        case JwtAlgorithm::kHs512: return "HS512";
    }
    return {};
}

void validateSecret(std::string_view secret) {
    if (secret.empty()) { throw std::invalid_argument("JWT secret must not be empty"); }
}

[[nodiscard]] std::pmr::string hmacSign(
    JwtAlgorithm algorithm,
    std::string_view secret,
    std::string_view data,
    std::pmr::memory_resource* resource) {
    validateSecret(secret);
    unsigned int length = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (HMAC(
            digestFor(algorithm),
            secret.data(),
            static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(),
            digest.data(),
            &length) == nullptr) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    return base64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), length),
        resource);
}

[[nodiscard]] bool constantTimeEquals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) { return false; }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

[[nodiscard]] std::int64_t epochSeconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point fromEpochSeconds(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

struct TokenParts {
    std::string_view header;
    std::string_view payload;
    std::string_view signature;
    std::string_view signingInput;
};

[[nodiscard]] TokenParts splitToken(std::string_view token) {
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
        throw std::invalid_argument("JWT token must have three sections");
    }
    return TokenParts{
        token.substr(0, first),
        token.substr(first + 1, second - first - 1),
        token.substr(second + 1),
        token.substr(0, second)};
}

// Skip JSON whitespace (SP, HT, LF, CR).
[[nodiscard]] std::size_t jwtSkipWs(std::string_view s, std::size_t pos) noexcept {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

// Advance past one JSON string starting at s[pos] == '"'.
// When out != nullptr, append the decoded (unescaped) content.
// Returns the position after the closing '"', or s.size() on an unterminated string.
[[nodiscard]] std::size_t jwtScanString(
    std::string_view s,
    std::size_t pos,
    std::pmr::string* out) noexcept {
    if (pos >= s.size() || s[pos] != '"') {
        return s.size();
    }
    ++pos;
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') {
            return pos;
        }
        if (c == '\\' && pos < s.size()) {
            if (out != nullptr) {
                switch (s[pos]) {
                    case '"': case '\\': case '/': out->push_back(s[pos]); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'n': out->push_back('\n'); break;
                    case 'r': out->push_back('\r'); break;
                    case 't': out->push_back('\t'); break;
                    default:  out->push_back(s[pos]); break;
                }
            }
            ++pos;
        } else if (c != '\\' && out != nullptr) {
            out->push_back(c);
        }
    }
    return s.size();
}

// Skip past one scalar JSON value (string, number, bool, null).
[[nodiscard]] std::size_t jwtSkipValue(std::string_view s, std::size_t pos) noexcept {
    pos = jwtSkipWs(s, pos);
    if (pos >= s.size()) {
        return pos;
    }
    if (s[pos] == '"') {
        return jwtScanString(s, pos, nullptr);
    }
    while (pos < s.size() &&
           s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
           s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' && s[pos] != '\r') {
        ++pos;
    }
    return pos;
}

// Shared top-level key scanner: advances pos to the value position for `key`,
// or returns false if not found. Properly traverses the object structure so
// that patterns like "key": inside a string value cannot produce false matches.
[[nodiscard]] static bool jwtFindMember(
    std::string_view json,
    std::string_view key,
    std::size_t& pos) {
    pos = jwtSkipWs(json, 0);
    if (pos >= json.size() || json[pos] != '{') {
        return false;
    }
    ++pos;
    for (;;) {
        pos = jwtSkipWs(json, pos);
        if (pos >= json.size() || json[pos] != '"') {
            return false;
        }
        const auto keyStart = pos + 1;
        pos = jwtScanString(json, pos, nullptr);
        // pos <= keyStart means jwtScanString hit end-of-input before the closing '"'
        if (pos <= keyStart) {
            return false;
        }
        const auto foundKey = json.substr(keyStart, pos - 1 - keyStart);
        pos = jwtSkipWs(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            return false;
        }
        ++pos;
        pos = jwtSkipWs(json, pos);
        if (foundKey == key) {
            return true;
        }
        pos = jwtSkipValue(json, pos);
        pos = jwtSkipWs(json, pos);
        if (pos >= json.size() || json[pos] == '}') {
            return false;
        }
        if (json[pos] != ',') {
            return false;
        }
        ++pos;
    }
}

// Returns the raw (unescaped) bytes of the string value for `key`, or empty.
[[nodiscard]] std::string_view findJsonString(std::string_view json, std::string_view key) {
    std::size_t pos = 0;
    if (!jwtFindMember(json, key, pos) || pos >= json.size() || json[pos] != '"') {
        return {};
    }
    const auto valueStart = pos + 1;
    const auto afterValue = jwtScanString(json, pos, nullptr);
    return afterValue > valueStart ? json.substr(valueStart, afterValue - 1 - valueStart) : std::string_view{};
}

[[nodiscard]] std::optional<std::int64_t> findJsonInteger(std::string_view json, std::string_view key) {
    std::size_t pos = 0;
    if (!jwtFindMember(json, key, pos)) {
        return std::nullopt;
    }
    std::int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(
        json.data() + pos, json.data() + json.size(), parsed);
    if (ec != std::errc{} || ptr == json.data() + pos) {
        return std::nullopt;
    }
    return parsed;
}

// Finds `key` in a flat JSON object and decodes (unescapes) its string value
// into `target`. No-op when the key is absent or the value is not a string.
void copyJsonString(std::pmr::string& target, std::string_view json, std::string_view key) {
    std::size_t pos = 0;
    if (!jwtFindMember(json, key, pos) || pos >= json.size() || json[pos] != '"') {
        return;
    }
    target.clear();
    jwtScanString(json, pos, &target);
}

}  // namespace

struct JwtPayloadAccess final {
    static JwtPayload decodePayloadJson(std::string_view json, std::pmr::memory_resource* resource) {
        auto* resolved = resourceOrDefault(resource);
        JwtPayload payload(resolved);
        copyJsonString(payload.issuer_, json, "iss");
        copyJsonString(payload.subject_, json, "sub");
        copyJsonString(payload.audience_, json, "aud");
        copyJsonString(payload.id_, json, "jti");
        if (const auto exp = findJsonInteger(json, "exp")) { payload.expiresAt_ = fromEpochSeconds(*exp); }
        if (const auto nbf = findJsonInteger(json, "nbf")) { payload.notBefore_ = fromEpochSeconds(*nbf); }
        if (const auto iat = findJsonInteger(json, "iat")) { payload.issuedAt_ = fromEpochSeconds(*iat); }

        // Collect non-reserved custom string claims with proper JSON traversal
        // and escape-sequence decoding via jwtScanString.
        std::size_t pos = jwtSkipWs(json, 0);
        if (pos >= json.size() || json[pos] != '{') {
            return payload;
        }
        ++pos;
        for (;;) {
            pos = jwtSkipWs(json, pos);
            if (pos >= json.size() || json[pos] != '"') {
                break;
            }
            std::pmr::string key(resolved);
            pos = jwtScanString(json, pos, &key);
            pos = jwtSkipWs(json, pos);
            if (pos >= json.size() || json[pos] != ':') {
                break;
            }
            ++pos;
            pos = jwtSkipWs(json, pos);
            if (!isReservedClaim(key) && pos < json.size() && json[pos] == '"') {
                std::pmr::string value(resolved);
                pos = jwtScanString(json, pos, &value);
                payload.claims_.push_back(JwtClaim{std::move(key), std::move(value)});
            } else {
                pos = jwtSkipValue(json, pos);
            }
            pos = jwtSkipWs(json, pos);
            if (pos >= json.size() || json[pos] == '}') {
                break;
            }
            if (json[pos] != ',') {
                break;
            }
            ++pos;
        }
        return payload;
    }
};

JwtPayload::JwtPayload(std::pmr::memory_resource* resource)
    : issuer_(resourceOrDefault(resource)),
      subject_(issuer_.get_allocator().resource()),
      audience_(issuer_.get_allocator().resource()),
      id_(issuer_.get_allocator().resource()),
      claims_(issuer_.get_allocator().resource()) {}

std::string_view JwtPayload::issuer() const noexcept { return issuer_; }
std::string_view JwtPayload::subject() const noexcept { return subject_; }
std::string_view JwtPayload::audience() const noexcept { return audience_; }
std::string_view JwtPayload::id() const noexcept { return id_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::expiresAt() const noexcept { return expiresAt_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::notBefore() const noexcept { return notBefore_; }
std::optional<std::chrono::system_clock::time_point> JwtPayload::issuedAt() const noexcept { return issuedAt_; }
std::span<const JwtClaim> JwtPayload::claims() const noexcept { return {claims_.data(), claims_.size()}; }

std::optional<std::string_view> JwtPayload::claim(std::string_view name) const noexcept {
    for (const auto& item : claims_) {
        if (item.name == name) { return std::string_view(item.value); }
    }
    return std::nullopt;
}

std::pmr::string jwtSign(const JwtSignOptions& options, std::pmr::memory_resource* resource) {
    auto* resolved = resourceOrDefault(resource);
    const auto now = std::chrono::system_clock::now();
    std::pmr::string header(resolved);
    header.append("{\"alg\":");
    appendJsonEscaped(header, algorithmName(options.algorithm));
    header.append(",\"typ\":\"JWT\"}");

    std::pmr::string payload(resolved);
    payload.push_back('{');
    bool first = true;
    if (!options.issuer.empty()) { appendJsonMember(payload, first, "iss", options.issuer); }
    if (!options.subject.empty()) { appendJsonMember(payload, first, "sub", options.subject); }
    if (!options.audience.empty()) { appendJsonMember(payload, first, "aud", options.audience); }
    if (!options.id.empty()) { appendJsonMember(payload, first, "jti", options.id); }
    appendJsonMember(payload, first, "iat", epochSeconds(now));
    if (options.expiresIn.count() > 0) { appendJsonMember(payload, first, "exp", epochSeconds(now + options.expiresIn)); }
    if (options.notBeforeDelay.count() > 0) { appendJsonMember(payload, first, "nbf", epochSeconds(now + options.notBeforeDelay)); }
    for (const auto& claim : options.claims) {
        if (claim.name.empty() || isReservedClaim(claim.name)) {
            throw std::invalid_argument("JWT custom claim name is empty or reserved");
        }
        appendJsonMember(payload, first, claim.name, claim.value);
    }
    payload.push_back('}');

    auto encodedHeader = base64UrlEncode(header, resolved);
    auto encodedPayload = base64UrlEncode(payload, resolved);
    std::pmr::string signingInput(resolved);
    signingInput.append(encodedHeader);
    signingInput.push_back('.');
    signingInput.append(encodedPayload);
    auto signature = hmacSign(options.algorithm, options.secret, signingInput, resolved);
    signingInput.push_back('.');
    signingInput.append(signature);
    return signingInput;
}

JwtPayload jwtVerify(std::string_view token, const JwtVerifyOptions& options, std::pmr::memory_resource* resource) {
    const auto parts = splitToken(token);
    const auto expected = hmacSign(options.algorithm, options.secret, parts.signingInput, std::pmr::get_default_resource());
    if (!constantTimeEquals(expected, parts.signature)) { throw std::runtime_error("JWT signature verification failed"); }
    const auto header = base64UrlDecode(parts.header, std::pmr::get_default_resource());
    if (findJsonString(header, "alg") != algorithmName(options.algorithm)) { throw std::runtime_error("JWT algorithm mismatch"); }
    const auto payloadJson = base64UrlDecode(parts.payload, std::pmr::get_default_resource());
    auto payload = JwtPayloadAccess::decodePayloadJson(payloadJson, resource);
    const auto now = std::chrono::system_clock::now();
    if (options.requireExpiration && !payload.expiresAt()) { throw std::runtime_error("JWT token is missing exp claim"); }
    if (payload.expiresAt() && now > *payload.expiresAt() + options.leeway) { throw std::runtime_error("JWT token is expired"); }
    if (payload.notBefore() && now + options.leeway < *payload.notBefore()) { throw std::runtime_error("JWT token is not yet valid"); }
    if (!options.issuer.empty() && payload.issuer() != options.issuer) { throw std::runtime_error("JWT issuer mismatch"); }
    if (!options.subject.empty() && payload.subject() != options.subject) { throw std::runtime_error("JWT subject mismatch"); }
    if (!options.audience.empty() && payload.audience() != options.audience) { throw std::runtime_error("JWT audience mismatch"); }
    return payload;
}

JwtPayload jwtDecodeUnverified(std::string_view token, std::pmr::memory_resource* resource) {
    const auto parts = splitToken(token);
    const auto payloadJson = base64UrlDecode(parts.payload, std::pmr::get_default_resource());
    return JwtPayloadAccess::decodePayloadJson(payloadJson, resource);
}

std::optional<std::string_view> jwtBearerToken(std::string_view authorization) noexcept {
    constexpr std::string_view prefix = "Bearer ";
    if (authorization.size() <= prefix.size()) { return std::nullopt; }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto left = authorization[i] >= 'A' && authorization[i] <= 'Z' ? authorization[i] + 32 : authorization[i];
        const auto right = prefix[i] >= 'A' && prefix[i] <= 'Z' ? prefix[i] + 32 : prefix[i];
        if (left != right) { return std::nullopt; }
    }
    return authorization.substr(prefix.size());
}

}  // namespace ruvia
