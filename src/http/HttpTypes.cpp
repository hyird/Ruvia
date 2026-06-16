#include "ruvia/http/HttpTypes.h"

namespace ruvia {

const std::filesystem::path& detail::fileTokenPath(const FileToken& token) noexcept {
    return token.borrowedPath_ == nullptr ? token.ownedPath_ : *token.borrowedPath_;
}

FileToken::FileToken(const FileToken& other)
    : borrowedPath_(other.borrowedPath_) {
    if (other.borrowedPath_ == nullptr) {
        ownedPath_ = other.ownedPath_;
    }
}

FileToken& FileToken::operator=(const FileToken& other) {
    if (this == &other) {
        return *this;
    }

    borrowedPath_ = other.borrowedPath_;
    if (other.borrowedPath_ == nullptr) {
        ownedPath_ = other.ownedPath_;
    } else {
        ownedPath_.clear();
    }
    return *this;
}

MultipartPart::MultipartPart(std::pmr::memory_resource* resource)
    : name(resource), filename(resource), contentType(resource) {}

}  // namespace ruvia
