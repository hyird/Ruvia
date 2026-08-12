#pragma once

// Streaming JSON output for responses whose shape is only known at run time.
//
// Ruvia's model JSON (RUVIA_RESPONSE_MODEL + c.json(value)) covers every response whose
// shape is known when the code is written, and it stays the preferred path: the
// schema is compile-time, so serialization allocates nothing it cannot size in
// advance. But some responses genuinely are not statically shaped -- an error
// envelope's details, a diagnostics dump, an aggregation keyed by whatever the
// data contained. Before this, those had exactly one option: concatenate the
// JSON by hand, and own the escaping. HttpErrorInfo::detailsJson() taking a
// bare JSON string is that hole already reaching the public API.
//
// This is deliberately NOT a JSON DOM. There is no node type, nothing is
// buffered into a tree, and no runtime schema exists: writers append directly
// to one output string, in order. What they own is exactly what hand-written
// concatenation gets wrong -- string escaping, comma placement, and bracket
// balance.

#include "ruvia/web/detail/json/JsonEscape.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia {

class JsonObjectWriter;
class JsonArrayWriter;

namespace detail {

// Shared bookkeeping for both writer kinds: where to append, whether a
// separating comma is due, and whether a nested writer currently owns the
// output. A parent must not be written to while a child is open -- the bytes
// would interleave into the child's braces -- so the parent refuses instead of
// silently producing corrupt JSON.
class JsonWriterScope {
public:
    JsonWriterScope(const JsonWriterScope&) = delete;
    JsonWriterScope& operator=(const JsonWriterScope&) = delete;
    JsonWriterScope(JsonWriterScope&&) = delete;
    JsonWriterScope& operator=(JsonWriterScope&&) = delete;

protected:
    JsonWriterScope(std::pmr::string& output, JsonWriterScope* parent) noexcept
        : output_(&output),
          parent_(parent) {
        if (parent_ != nullptr) {
            parent_->childOpen_ = true;
        }
    }

    ~JsonWriterScope() noexcept {
        if (parent_ != nullptr) {
            parent_->childOpen_ = false;
        }
    }

    void requireWritable() const {
        if (childOpen_) {
            throw std::logic_error("JSON writer has an open nested writer");
        }
    }

    void separate() {
        requireWritable();
        if (!first_) {
            output_->push_back(',');
        }
        first_ = false;
    }

    void appendScalar(std::string_view value) {
        detail::appendJsonString(*output_, value);
    }

    void appendScalar(bool value) {
        output_->append(value ? "true" : "false");
    }

    template <typename T>
        requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
    void appendScalar(T value) {
        char buffer[32];
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output_->append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }

    template <std::floating_point T>
    void appendScalar(T value) {
        // JSON has no infinity or NaN. Emitting them verbatim would produce a
        // body no conforming parser accepts, so they become null -- the same
        // choice the model writer makes.
        if (!std::isfinite(value)) {
            output_->append("null");
            return;
        }
        char buffer[64];
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output_->append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }

    std::pmr::string* output_;
    JsonWriterScope* parent_{nullptr};
    bool first_{true};
    bool childOpen_{false};
};

}  // namespace detail

// Writes one JSON object. Emits '{' on construction and '}' on destruction, so
// the object is closed by scope exit on every path, including an exception.
class JsonObjectWriter final : private detail::JsonWriterScope {
public:
    explicit JsonObjectWriter(std::pmr::string& output)
        : JsonObjectWriter(output, nullptr) {}

    ~JsonObjectWriter() noexcept {
        output_->push_back('}');
    }

    void add(std::string_view key, std::string_view value) {
        writeKey(key);
        appendScalar(value);
    }

    // Without this, a string literal would select the bool overload: pointer to
    // bool is a standard conversion, pointer to string_view is user-defined.
    void add(std::string_view key, const char* value) {
        add(key, std::string_view(value));
    }

    void add(std::string_view key, bool value) {
        writeKey(key);
        appendScalar(value);
    }

    template <typename T>
        requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
    void add(std::string_view key, T value) {
        writeKey(key);
        appendScalar(value);
    }

    template <std::floating_point T>
    void add(std::string_view key, T value) {
        writeKey(key);
        appendScalar(value);
    }

    void addNull(std::string_view key) {
        writeKey(key);
        output_->append("null");
    }

    // Splices a RUVIA_RESPONSE_MODEL value in as a nested object, so a statically shaped
    // subtree keeps using the compile-time schema path.
    template <typename T>
    void addModel(std::string_view key, const T& model) {
        writeKey(key);
        detail::appendJsonValue(*output_, model);
    }

    // The returned writer owns the output until it is destroyed; this writer
    // rejects writes for that whole scope.
    [[nodiscard]] JsonObjectWriter beginObject(std::string_view key) {
        writeKey(key);
        return JsonObjectWriter(*output_, this);
    }

    [[nodiscard]] JsonArrayWriter beginArray(std::string_view key);

private:
    friend class JsonArrayWriter;

    JsonObjectWriter(std::pmr::string& output, detail::JsonWriterScope* parent)
        : JsonWriterScope(output, parent) {
        output_->push_back('{');
    }

    void writeKey(std::string_view key) {
        separate();
        detail::appendJsonString(*output_, key);
        output_->push_back(':');
    }
};

// Writes one JSON array, with the same scope-bound bracket guarantee.
class JsonArrayWriter final : private detail::JsonWriterScope {
public:
    explicit JsonArrayWriter(std::pmr::string& output)
        : JsonArrayWriter(output, nullptr) {}

    ~JsonArrayWriter() noexcept {
        output_->push_back(']');
    }

    void add(std::string_view value) {
        separate();
        appendScalar(value);
    }

    void add(const char* value) {
        add(std::string_view(value));
    }

    void add(bool value) {
        separate();
        appendScalar(value);
    }

    template <typename T>
        requires(std::integral<T> && !std::same_as<std::remove_cv_t<T>, bool>)
    void add(T value) {
        separate();
        appendScalar(value);
    }

    template <std::floating_point T>
    void add(T value) {
        separate();
        appendScalar(value);
    }

    void addNull() {
        separate();
        output_->append("null");
    }

    template <typename T>
    void addModel(const T& model) {
        separate();
        detail::appendJsonValue(*output_, model);
    }

    [[nodiscard]] JsonObjectWriter beginObject() {
        separate();
        return JsonObjectWriter(*output_, this);
    }

    [[nodiscard]] JsonArrayWriter beginArray() {
        separate();
        return JsonArrayWriter(*output_, this);
    }

private:
    friend class JsonObjectWriter;

    JsonArrayWriter(std::pmr::string& output, detail::JsonWriterScope* parent)
        : JsonWriterScope(output, parent) {
        output_->push_back('[');
    }
};

inline JsonArrayWriter JsonObjectWriter::beginArray(std::string_view key) {
    writeKey(key);
    return JsonArrayWriter(*output_, this);
}

}  // namespace ruvia
