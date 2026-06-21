#pragma once

#include <filesystem>
#include <utility>

namespace ruvia {

class Context;
class FileToken;
class HttpResponse;
class StaticRoot;

namespace detail {

const std::filesystem::path& fileTokenPath(const ruvia::FileToken& token) noexcept;

}  // namespace detail

class FileToken final {
public:
    FileToken() = default;
    FileToken(const FileToken& other);
    FileToken& operator=(const FileToken& other);
    FileToken(FileToken&&) noexcept = default;
    FileToken& operator=(FileToken&&) noexcept = default;

    [[nodiscard]] bool empty() const noexcept {
        return detail::fileTokenPath(*this).empty();
    }

private:
    friend class Context;
    friend class StaticRoot;
    friend class HttpResponse;
    friend const std::filesystem::path& detail::fileTokenPath(const FileToken& token) noexcept;

    [[nodiscard]] static FileToken borrow(const FileToken& token) noexcept {
        FileToken borrowed;
        borrowed.borrowedPath_ = token.borrowedPath_ == nullptr ? &token.ownedPath_ : token.borrowedPath_;
        return borrowed;
    }

    explicit FileToken(std::filesystem::path path) : ownedPath_(std::move(path)) {}

    std::filesystem::path ownedPath_;
    const std::filesystem::path* borrowedPath_{nullptr};
};

}  // namespace ruvia
