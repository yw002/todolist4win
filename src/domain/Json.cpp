#include "domain/Json.h"

#include <cstdio>
#include <cstdint>
#include <utility>

namespace todo::json {
namespace {

[[nodiscard]] AppError Invalid(const char* message) { return AppError{ErrorCode::InvalidInput, message, 0}; }

[[nodiscard]] bool IsContinuation(unsigned char value) noexcept { return value >= 0x80U && value <= 0xBFU; }

void AppendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint <= 0x7FU) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

[[nodiscard]] int HexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser {
public:
    Parser(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

    [[nodiscard]] Result<Value> Run() {
        if (text_.size() > limits_.maxTotalBytes) {
            return Result<Value>::Failure(AppError{ErrorCode::BodyTooLarge, "JSON 文档超过 256 KB 上限", 0});
        }
        if (!IsValidUtf8(text_)) return Result<Value>::Failure(Invalid("JSON 含非法 UTF-8 编码"));
        Value root;
        if (!ParseValue(root, 0)) return Result<Value>::Failure(error_);
        SkipWhitespace();
        if (pos_ != text_.size()) {
            Fail("JSON 末尾存在多余字符");
            return Result<Value>::Failure(error_);
        }
        return Result<Value>::Success(std::move(root));
    }

private:
    void Fail(const char* message) { error_ = Invalid(message); }
    [[nodiscard]] bool Available() const noexcept { return pos_ < text_.size(); }
    [[nodiscard]] char Peek() const noexcept { return Available() ? text_[pos_] : '\0'; }

    void SkipWhitespace() noexcept {
        while (Available()) {
            const char value = text_[pos_];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
            ++pos_;
        }
    }

    [[nodiscard]] bool Consume(char expected) {
        if (!Available() || text_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    [[nodiscard]] bool ConsumeWord(std::string_view word) {
        if (text_.size() - pos_ < word.size() || text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    [[nodiscard]] bool ParseValue(Value& out, int depth) {
        if (depth > static_cast<int>(limits_.maxDepth)) { Fail("JSON 嵌套层数超过上限"); return false; }
        SkipWhitespace();
        const std::size_t start = pos_;
        bool parsed = false;
        const char lead = Peek();
        if (lead == 'n') {
            if (ConsumeWord("null")) { out.kind = Kind::Null; parsed = true; }
        } else if (lead == 't' || lead == 'f') {
            const bool value = lead == 't';
            if (ConsumeWord(value ? "true" : "false")) { out.kind = Kind::Bool; out.boolean = value; parsed = true; }
        } else if (lead == '"') {
            out.kind = Kind::String;
            parsed = ParseString(out.stringValue);
        } else if (lead == '[') {
            parsed = ParseArray(out, depth + 1);
        } else if (lead == '{') {
            parsed = ParseObject(out, depth + 1);
        } else if (lead == '-' || (lead >= '0' && lead <= '9')) {
            parsed = ParseNumber(out);
        } else {
            Fail("JSON 值格式无效");
        }
        if (!parsed) { if (error_.code == ErrorCode::None) Fail("JSON 值格式无效"); return false; }
        out.raw = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    [[nodiscard]] bool ParseHex4(std::uint32_t& codePoint) {
        if (text_.size() - pos_ < 4) { Fail("\\u 转义不完整"); return false; }
        codePoint = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexDigit(text_[pos_ + i]);
            if (digit < 0) { Fail("\\u 转义含非法十六进制字符"); return false; }
            codePoint = (codePoint << 4) | static_cast<std::uint32_t>(digit);
        }
        pos_ += 4;
        return true;
    }

    [[nodiscard]] bool ParseString(std::string& out) {
        if (!Consume('"')) { Fail("JSON 字符串缺少起始引号"); return false; }
        const std::size_t rawStart = pos_ - 1;
        out.clear();
        while (Available()) {
            const unsigned char value = static_cast<unsigned char>(text_[pos_]);
            if (value == '"') {
                ++pos_;
                if (out.size() > limits_.maxStringBytes || pos_ - rawStart > limits_.maxStringBytes) { Fail("JSON 字符串超过长度上限"); return false; }
                return true;
            }
            if (value < 0x20U) { Fail("JSON 字符串含未转义控制字符"); return false; }
            if (value != '\\') { out.push_back(static_cast<char>(value)); ++pos_; }
            else {
                ++pos_;
                if (!Available()) { Fail("JSON 转义序列不完整"); return false; }
                const char escaped = text_[pos_++];
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t codePoint = 0;
                        if (!ParseHex4(codePoint)) return false;
                        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
                            if (text_.size() - pos_ < 6 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') { Fail("高代理项后缺少低代理项"); return false; }
                            pos_ += 2;
                            std::uint32_t low = 0;
                            if (!ParseHex4(low)) return false;
                            if (low < 0xDC00U || low > 0xDFFFU) { Fail("低代理项取值非法"); return false; }
                            codePoint = 0x10000U + ((codePoint - 0xD800U) << 10) + (low - 0xDC00U);
                        } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
                            Fail("出现孤立低代理项");
                            return false;
                        }
                        AppendUtf8(out, codePoint);
                        break;
                    }
                    default: Fail("JSON 含未知转义序列"); return false;
                }
            }
            if (out.size() > limits_.maxStringBytes) { Fail("JSON 字符串超过长度上限"); return false; }
        }
        Fail("JSON 字符串未闭合");
        return false;
    }

    [[nodiscard]] bool ParseNumber(Value& out) {
        const std::size_t start = pos_;
        if (Peek() == '-') ++pos_;
        if (!Available()) { Fail("JSON 数字不完整"); return false; }
        if (Peek() == '0') {
            ++pos_;
            if (Available() && Peek() >= '0' && Peek() <= '9') { Fail("JSON 数字含多余前导零"); return false; }
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (Available() && Peek() >= '0' && Peek() <= '9') ++pos_;
        } else {
            Fail("JSON 数字格式无效");
            return false;
        }
        if (Available() && Peek() == '.') {
            ++pos_;
            if (!Available() || Peek() < '0' || Peek() > '9') { Fail("JSON 小数部分缺少数字"); return false; }
            while (Available() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        if (Available() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (Available() && (Peek() == '+' || Peek() == '-')) ++pos_;
            if (!Available() || Peek() < '0' || Peek() > '9') { Fail("JSON 指数部分缺少数字"); return false; }
            while (Available() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        out.kind = Kind::Number;
        out.numberText = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    [[nodiscard]] bool ParseArray(Value& out, int depth) {
        out.kind = Kind::Array;
        ++pos_;  // '['
        SkipWhitespace();
        if (Consume(']')) return true;
        for (;;) {
            Value element;
            if (!ParseValue(element, depth)) return false;
            out.elements.push_back(std::move(element));
            if (out.elements.size() > limits_.maxArrayElements) { Fail("JSON 数组元素超过上限"); return false; }
            SkipWhitespace();
            if (Consume(',')) { SkipWhitespace(); continue; }
            if (Consume(']')) return true;
            Fail("JSON 数组缺少 ']' 或 ','");
            return false;
        }
    }

    [[nodiscard]] bool ParseObject(Value& out, int depth) {
        out.kind = Kind::Object;
        ++pos_;  // '{'
        SkipWhitespace();
        if (Consume('}')) return true;
        for (;;) {
            SkipWhitespace();
            const std::size_t memberStart = pos_;
            std::string key;
            if (!Available() || Peek() != '"') { Fail("JSON 对象键必须是字符串"); return false; }
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) { Fail("JSON 对象键后缺少 ':'"); return false; }
            Value member;
            if (!ParseValue(member, depth)) return false;
            out.memberRaws.push_back(std::string(text_.substr(memberStart, pos_ - memberStart)));
            out.memberKeys.push_back(std::move(key));
            out.memberValues.push_back(std::move(member));
            if (out.memberKeys.size() > limits_.maxArrayElements) { Fail("JSON 对象成员超过上限"); return false; }
            SkipWhitespace();
            if (Consume(',')) { SkipWhitespace(); continue; }
            if (Consume('}')) return true;
            Fail("JSON 对象缺少 '}' 或 ','");
            return false;
        }
    }

    std::string_view text_;
    Limits limits_;
    std::size_t pos_{0};
    AppError error_{};
};

}  // namespace

bool IsValidUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7FU) { ++index; continue; }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            if (index + 1 >= text.size() || !IsContinuation(static_cast<unsigned char>(text[index + 1]))) return false;
            index += 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            if (index + 2 >= text.size()) return false;
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            const unsigned char third = static_cast<unsigned char>(text[index + 2]);
            if (!IsContinuation(third)) return false;
            if (lead == 0xE0U && second < 0xA0U) return false;             // overlong
            if (lead == 0xEDU && second > 0x9FU) return false;             // UTF-16 surrogate
            if (!IsContinuation(second)) return false;
            index += 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            if (index + 3 >= text.size()) return false;
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            const unsigned char third = static_cast<unsigned char>(text[index + 2]);
            const unsigned char fourth = static_cast<unsigned char>(text[index + 3]);
            if (!IsContinuation(second) || !IsContinuation(third) || !IsContinuation(fourth)) return false;
            if (lead == 0xF0U && second < 0x90U) return false;             // overlong
            if (lead == 0xF4U && second > 0x8FU) return false;             // > U+10FFFF
            index += 4;
        } else {
            return false;
        }
    }
    return true;
}

const Value* Value::Find(std::string_view key) const noexcept {
    for (std::size_t i = 0; i < memberKeys.size() && i < memberValues.size(); ++i) {
        if (memberKeys[i] == key) return &memberValues[i];
    }
    return nullptr;
}

const std::string* Value::FindString(std::string_view key) const noexcept {
    const Value* found = Find(key);
    return found != nullptr && found->kind == Kind::String ? &found->stringValue : nullptr;
}

Result<Value> Parse(const std::string& text, const Limits& limits) {
    Parser parser(text, limits);
    return parser.Run();
}

std::string Quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    char buffer[8]{};
    for (const char raw : text) {
        const unsigned char value = static_cast<unsigned char>(raw);
        switch (value) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (value < 0x20U) {
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(value));
                    out += buffer;
                } else {
                    out.push_back(raw);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string Dump(const Value& value) {
    switch (value.kind) {
        case Kind::Null: return "null";
        case Kind::Bool: return value.boolean ? "true" : "false";
        case Kind::Number: return value.numberText.empty() ? "0" : value.numberText;
        case Kind::String: return Quote(value.stringValue);
        case Kind::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.elements.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += Dump(value.elements[i]);
            }
            out.push_back(']');
            return out;
        }
        case Kind::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < value.memberKeys.size() && i < value.memberValues.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += Quote(value.memberKeys[i]);
                out.push_back(':');
                out += Dump(value.memberValues[i]);
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

}  // namespace todo::json
