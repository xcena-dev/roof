// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/base.cpp -- see base.hpp.

#include "policy/base.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "model.hpp"
#include "wire/protocol.hpp"

namespace fsdaemon::policy
{
namespace
{

// ── The JSON this layer reads, and the one piece it writes ──────────
// Nothing outside this file names a JSON value: readDecision takes a document as text.
class Value
{
public:
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0};
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // One scalar, built where the reader recognised it. The three exist so no caller sets a kind
    // and its payload in two statements, where one could be forgotten.
    [[nodiscard]] static Value makeBool(bool truth)
    {
        Value made{};
        made.kind = Kind::Bool;
        made.boolean = truth;
        return made;
    }
    [[nodiscard]] static Value makeNumber(double number)
    {
        Value made{};
        made.kind = Kind::Number;
        made.number = number;
        return made;
    }
    [[nodiscard]] static Value makeString(std::string text)
    {
        Value made{};
        made.kind = Kind::String;
        made.text = std::move(text);
        return made;
    }

    [[nodiscard]] bool isObject() const noexcept
    {
        return kind == Kind::Object;
    }
    [[nodiscard]] bool isArray() const noexcept
    {
        return kind == Kind::Array;
    }
    [[nodiscard]] bool isString() const noexcept
    {
        return kind == Kind::String;
    }
    [[nodiscard]] bool isBool() const noexcept
    {
        return kind == Kind::Bool;
    }
    [[nodiscard]] bool isNumber() const noexcept
    {
        return kind == Kind::Number;
    }

    // The member named @key, or nullptr when this is not an object or has no such member.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept
    {
        if (!isObject())
        {
            return nullptr;
        }
        for (const auto& [name, value] : object)
        {
            if (name == key)
            {
                return &value;
            }
        }
        return nullptr;
    }

    // The text of the string member named @key, or @fallback when there is none or it is not a string.
    [[nodiscard]] std::string stringOr(std::string_view key, std::string_view fallback) const
    {
        const auto* named = find(key);
        if (named == nullptr || !named->isString())
        {
            return std::string{fallback};
        }
        return named->text;
    }
};

class Reader
{
public:
    explicit Reader(std::string_view text) noexcept
        : text_{text}
    {
    }

    [[nodiscard]] std::optional<Value> read()
    {
        skipSpace();
        auto value = readValue();
        if (!value)
        {
            return std::nullopt;
        }
        skipSpace();
        if (position_ != text_.size())
        {
            return std::nullopt;
        }
        return value;
    }

private:
    // Moves past every character from here on that is one of @chars.
    void skipAny(std::string_view chars) noexcept
    {
        const auto stop = text_.find_first_not_of(chars, position_);
        position_ = (stop == std::string_view::npos) ? text_.size() : stop;
    }

    void skipSpace() noexcept
    {
        skipAny(Whitespace);
    }

    [[nodiscard]] bool consume(char expected) noexcept
    {
        if (position_ < text_.size() && text_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool matchWord(std::string_view word) noexcept
    {
        if (text_.substr(position_).compare(0, word.size(), word) == 0)
        {
            position_ += word.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<Value> readValue()
    {
        skipSpace();
        if (position_ >= text_.size())
        {
            return std::nullopt;
        }
        const auto here = text_[position_];
        if (here == '{' || here == '[')
        {
            if (depth_ == MaxDepth)
            {
                return std::nullopt;
            }
            ++depth_;
            auto nested = (here == '{') ? readObject() : readArray();
            --depth_;
            return nested;
        }
        switch (here)
        {
            case '"':
                return readStringValue();
            case 't':
            case 'f':
                return readBool();
            case 'n':
                return matchWord("null") ? std::optional<Value>{Value{}} : std::nullopt;
            default:
                return readNumber();
        }
    }

    [[nodiscard]] std::optional<Value> readBool()
    {
        if (matchWord("true"))
        {
            return Value::makeBool(true);
        }
        if (matchWord("false"))
        {
            return Value::makeBool(false);
        }
        return std::nullopt;
    }

    // The one escape @out gets from the backslash already consumed, or false on an escape this
    // reader does not know. Split out of readRawString() so a \u sequence nests no deeper than an
    // ordinary one.
    [[nodiscard]] bool readEscape(std::string& out)
    {
        if (position_ >= text_.size())
        {
            return false;
        }
        const auto escape = text_[position_++];
        switch (escape)
        {
            case '"':
                out.push_back('"');
                return true;
            case '\\':
                out.push_back('\\');
                return true;
            case '/':
                out.push_back('/');
                return true;
            case 'b':
                out.push_back('\b');
                return true;
            case 'f':
                out.push_back('\f');
                return true;
            case 'n':
                out.push_back('\n');
                return true;
            case 'r':
                out.push_back('\r');
                return true;
            case 't':
                out.push_back('\t');
                return true;
            case 'u':
            {
                const auto code = readHex4();
                if (!code)
                {
                    return false;
                }
                appendUtf8(out, *code);
                return true;
            }
            default:
                return false;
        }
    }

    [[nodiscard]] std::optional<std::string> readRawString()
    {
        if (!consume('"'))
        {
            return std::nullopt;
        }
        std::string out;
        while (position_ < text_.size())
        {
            const auto here = text_[position_++];
            if (here == '"')
            {
                return out;
            }
            if (here == '\\')
            {
                if (!readEscape(out))
                {
                    return std::nullopt;
                }
                continue;
            }
            out.push_back(here);
        }
        return std::nullopt;
    }

    // The four hex digits of a \u escape, or nullopt when fewer than four remain or one is not hex.
    [[nodiscard]] std::optional<char32_t> readHex4()
    {
        if (position_ + HexDigits > text_.size())
        {
            return std::nullopt;
        }
        const auto* first = text_.data() + position_;
        const auto* last = first + HexDigits;
        std::uint32_t code = 0;
        const auto result = std::from_chars(first, last, code, HexBase);
        if (result.ec != std::errc{} || result.ptr != last)
        {
            return std::nullopt;
        }
        position_ += HexDigits;
        return static_cast<char32_t>(code);
    }

    // UTF-8 for a code point of at most 16 bits, which is all a \u escape carries.
    static void appendUtf8(std::string& out, char32_t code)
    {
        constexpr char32_t OneByteLimit = 0x80;
        constexpr char32_t TwoByteLimit = 0x800;
        constexpr char32_t TwoByteLead = 0xC0;
        constexpr char32_t ThreeByteLead = 0xE0;
        constexpr char32_t ContinuationLead = 0x80;
        constexpr char32_t ContinuationMask = 0x3F;
        constexpr std::uint32_t ContinuationBits = 6;
        if (code < OneByteLimit)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < TwoByteLimit)
        {
            out.push_back(static_cast<char>(TwoByteLead | (code >> ContinuationBits)));
            out.push_back(static_cast<char>(ContinuationLead | (code & ContinuationMask)));
        }
        else
        {
            out.push_back(static_cast<char>(ThreeByteLead | (code >> (2 * ContinuationBits))));
            out.push_back(static_cast<char>(ContinuationLead | ((code >> ContinuationBits) & ContinuationMask)));
            out.push_back(static_cast<char>(ContinuationLead | (code & ContinuationMask)));
        }
    }

    [[nodiscard]] std::optional<Value> readStringValue()
    {
        auto raw = readRawString();
        if (!raw)
        {
            return std::nullopt;
        }
        return Value::makeString(std::move(*raw));
    }

    [[nodiscard]] std::optional<Value> readNumber()
    {
        const auto start = position_;
        skipAny(NumberChars);
        if (position_ == start)
        {
            return std::nullopt;
        }
        const auto token = text_.substr(start, position_ - start);
        auto parsed = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        // The whole token and not a prefix of it: "1-2" would parse as 1 and is not a number.
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
        {
            return std::nullopt;
        }
        return Value::makeNumber(parsed);
    }

    [[nodiscard]] std::optional<Value> readArray()
    {
        static_cast<void>(consume('['));
        Value value{};
        value.kind = Value::Kind::Array;
        skipSpace();
        if (consume(']'))
        {
            return value;
        }
        for (;;)
        {
            auto element = readValue();
            if (!element)
            {
                return std::nullopt;
            }
            value.array.push_back(std::move(*element));
            skipSpace();
            if (consume(','))
            {
                continue;
            }
            if (consume(']'))
            {
                return value;
            }
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<Value> readObject()
    {
        static_cast<void>(consume('{'));
        Value value{};
        value.kind = Value::Kind::Object;
        skipSpace();
        if (consume('}'))
        {
            return value;
        }
        for (;;)
        {
            skipSpace();
            auto key = readRawString();
            if (!key)
            {
                return std::nullopt;
            }
            skipSpace();
            if (!consume(':'))
            {
                return std::nullopt;
            }
            auto member = readValue();
            if (!member)
            {
                return std::nullopt;
            }
            // A key given twice is two answers to one question, and find() would take the
            // first. The engine never emits one, so the document is refused whole.
            if (value.find(*key) != nullptr)
            {
                return std::nullopt;
            }
            value.object.emplace_back(std::move(*key), std::move(*member));
            skipSpace();
            if (consume(','))
            {
                continue;
            }
            if (consume('}'))
            {
                return value;
            }
            return std::nullopt;
        }
    }

    // A document nests no deeper than this. The reader recurses, so a body with tens of thousands
    // of open braces would exhaust the stack instead of being refused.
    static constexpr std::uint32_t MaxDepth = 64;

    // The blanks JSON allows between tokens, and the characters a number is made of.
    static constexpr std::string_view Whitespace = " \t\n\r";
    static constexpr std::string_view NumberChars = "0123456789+-.eE";

    // A \u escape carries four hex digits.
    static constexpr std::uint32_t HexDigits = 4;
    static constexpr std::int32_t HexBase = 16;

    std::string_view text_;
    std::uint64_t position_{0};
    std::uint32_t depth_{0};
};

std::string quote(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const auto here : text)
    {
        switch (here)
        {
            case '"':
                out += R"(\")";
                break;
            case '\\':
                out += R"(\\)";
                break;
            case '\n':
                out += R"(\n)";
                break;
            case '\r':
                out += R"(\r)";
                break;
            case '\t':
                out += R"(\t)";
                break;
            default:
                if (const auto byte = static_cast<std::uint32_t>(static_cast<std::uint8_t>(here)); byte < ' ')
                {
                    static constexpr std::string_view Hex = "0123456789abcdef";
                    constexpr std::uint32_t NibbleBits = 4;
                    constexpr std::uint32_t NibbleMask = 0xF;
                    out += R"(\u00)";
                    out.push_back(Hex[(byte >> NibbleBits) & NibbleMask]);
                    out.push_back(Hex[byte & NibbleMask]);
                }
                else
                {
                    out.push_back(here);
                }
        }
    }
    out.push_back('"');
    return out;
}

// ── The decision a document carries ─────────────────────────────────
// The decision one already-parsed object carries.
Decision_t readDecisionObject(const Value& result)
{
    if (!result.isObject())
    {
        return makeDenial("policy result is not an object");
    }

    const auto* allow = result.find("allow");
    if (allow == nullptr || !allow->isBool())
    {
        return makeDenial("policy result is missing an allow");
    }

    const auto rule = result.stringOr("rule", {});

    if (!allow->boolean)
    {
        return makeDenial(result.stringOr("reason", "denied by policy"), rule);
    }

    const auto* perms = result.find("granted_perms");
    if (perms == nullptr || !perms->isArray())
    {
        return makeDenial("granted_perms is not a list", rule);
    }

    std::uint32_t mask = 0;
    for (const auto& name : perms->array)
    {
        if (!name.isString())
        {
            return makeDenial("granted_perms holds a non-string", rule);
        }
        const auto bit = wire::resolvePermBit(name.text);
        if (!bit)
        {
            return makeDenial("unknown perm: " + name.text, rule);
        }
        mask |= *bit;
    }

    std::int64_t expiry = 0;
    if (const auto* named = result.find("expiry_secs"); named != nullptr)
    {
        // A whole non-negative number that fits an int64, or an answer this side cannot carry.
        constexpr auto ExpiryMax = 9.0e18;
        if (!named->isNumber() || named->number < 0 || named->number > ExpiryMax ||
            named->number != std::floor(named->number))
        {
            return makeDenial("malformed expiry_secs", rule);
        }
        expiry = static_cast<std::int64_t>(named->number);
    }

    return Decision_t{true, mask, expiry, rule, ""};
}

}  // namespace

std::string buildPolicyInput(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                             std::string_view ownerRole)
{
    std::string out;
    out += R"({"consumer":{"spiffe_id":)";
    out += quote(consumer.spiffeId);
    out += R"(,"group":)";
    out += quote(consumer.group);
    out += R"(,"role":)";
    out += quote(consumer.role);
    out += R"(},"owner":{"group":)";
    out += quote(ownerGroup);
    out += R"(,"role":)";
    out += quote(ownerRole);
    out += "}}";
    return out;
}

Decision_t readDecision(std::string_view document, std::string_view wrapperKey)
{
    Reader reader{document};
    const auto parsed = reader.read();
    if (!parsed)
    {
        return makeDenial("policy returned a document this daemon could not read");
    }
    if (wrapperKey.empty())
    {
        return readDecisionObject(*parsed);
    }
    const auto* inner = parsed->find(wrapperKey);
    if (inner == nullptr)
    {
        return makeDenial(std::string{"policy response carried no "}.append(wrapperKey));
    }
    return readDecisionObject(*inner);
}

}  // namespace fsdaemon::policy
