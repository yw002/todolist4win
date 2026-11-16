#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/Result.h"

namespace todo::json {

// A deliberately small, self-contained JSON DOM. It is strict (rejects trailing garbage, invalid
// UTF-8, bad \u surrogate pairs and control characters in strings) and keeps the exact source slice
// of every value in `raw` so an unaware v1 writer can echo unknown fields back byte-for-byte.
enum class Kind { Null, Bool, Number, String, Array, Object };

// `std::vector<Value>` is legal with an incomplete element type since C++17, which is why the object
// members are stored as parallel vectors instead of a vector<pair<string, Value>> (std::pair needs a
// complete type at instantiation).
struct Value {
    Kind kind{Kind::Null};
    bool boolean{false};
    std::string numberText;              // exact literal; avoids double round-tripping
    std::string stringValue;             // decoded UTF-8 (String only)
    std::vector<Value> elements;         // Array
    std::vector<std::string> memberKeys; // Object, source order preserved
    std::vector<Value> memberValues;     // Object, same length as memberKeys
    std::vector<std::string> memberRaws; // Object, "key":value source slices, same length
    std::string raw;                     // verbatim source slice, no surrounding whitespace

    [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
    [[nodiscard]] const std::string* FindString(std::string_view key) const noexcept;
    [[nodiscard]] bool IsObject() const noexcept { return kind == Kind::Object; }
    [[nodiscard]] bool IsArray() const noexcept { return kind == Kind::Array; }
};

struct Limits {
    std::size_t maxDepth{8};
    std::size_t maxStringBytes{60000};
    std::size_t maxTotalBytes{262144};
    std::size_t maxArrayElements{256};
};

[[nodiscard]] Result<Value> Parse(const std::string& text, const Limits& limits = Limits{});
[[nodiscard]] std::string Dump(const Value& value);
[[nodiscard]] std::string Quote(std::string_view text);
[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

}  // namespace todo::json
