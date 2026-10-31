#pragma once

#include <string_view>

namespace ruvia {

// Request fields the HTTP protocol layer generates or interprets itself when it
// frames and transports a request: Host (RFC 9110 section 7.2), message framing
// (Content-Length, Transfer-Encoding, Trailer; RFC 9110 sections 6.5 and 8.6),
// Expect (RFC 9110 section 10.1.1), and connection management (Connection and
// the connection-specific Keep-Alive, Proxy-Connection, TE and Upgrade; RFC
// 9110 section 7.6.1). A component that builds a request on an application's
// behalf owns these fields and must not accept them from the application.
// Field names are matched case-insensitively.
[[nodiscard]] bool is_http_protocol_managed_request_field(std::string_view name) noexcept;

// 校验应用提供的单个出站请求首部，不分配内存。字段名须为 token，
// 字段值须符合 RFC 9110 §5.5 的规范化语法（不接受首尾 SP/HTAB），
// 并通过请求 writer 的已知字段发送方语义校验；未知合法扩展允许空值。
// 拒绝上面的协议托管字段，名称按大小写不敏感匹配；因此无需 HTTP 版本参数。
// 这不是响应首部或接收方校验器。调用方负责字段间重复及完整消息约束；
// 返回 true 不代表整个请求可发送，不保留输入或跨调用状态。
[[nodiscard]] bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept;

}  // namespace ruvia
