// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// common/kv_config.hpp -- the YAML subset this repository reads.
//
// Shared by the test harness, which reads per-machine facts, and by cmed, which reads its daemon
// settings. Header-only and in its own namespace, so including it couples neither side to the
// other's library.
//
// THE SUBSET. Nested maps by indentation, `key: value` scalars, and inline `[a, b]` sequences. A
// nested key is addressed by its path: `region.uri`. That path, and not the leaf name, is what
// keeps `area.path` and `log.path` apart.
//
// Anything outside the subset is refused with a line number rather than skipped. A skipped line is
// a setting that reads as its default while the file plainly says otherwise, and nothing in a run
// afterwards points back at the typo.

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/timing.hpp"

namespace kvconfig
{

// Carries the file and the line, because the whole reason to refuse rather than skip is to say
// where to look.
class ParseError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// True for std::chrono::duration and nothing else, so get() below can tell a count of units from a
// plain number without asking the caller which it wrote.
template <typename T>
inline constexpr bool IsDuration = false;
template <typename T_Rep, typename T_Period>
inline constexpr bool IsDuration<std::chrono::duration<T_Rep, T_Period>> = true;

class KeyValueConfig
{
public:
    // ── factories ──────────────────────────────────────────────────
    // Both refuse a file anyone but its owner can write, because a file someone else can rewrite
    // decides what the reader was configured to do. Whose job that is to fix is the operator's, and
    // the only thing a reader can usefully do about it is stop.
    //
    // Throws ParseError when the file cannot be opened. Use loadIfPresent where absence is a
    // normal answer rather than a fault.
    [[nodiscard]] static KeyValueConfig load(const std::string& path);

    // A missing file leaves every key absent, so each getter answers its fallback.
    [[nodiscard]] static KeyValueConfig loadIfPresent(const std::string& path);

    [[nodiscard]] static KeyValueConfig parse(std::istream& input, std::string origin);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] bool has(const std::string& key) const
    {
        return entries_.count(key) != 0;
    }

    [[nodiscard]] std::string getString(const std::string& key, const std::string& fallback = {}) const
    {
        const auto found = entries_.find(key);
        if (found == entries_.end())
        {
            return fallback;
        }
        return found->second;
    }

    // One read for every value a file spells one way. What it parses comes from the fallback, which is
    // the field being filled, so a key and its destination cannot disagree about the type or the unit.
    // A whole number past what that field holds is refused rather than truncated, and so is a duration
    // past what its rep counts.
    //
    // Absent from it on purpose: a mode shares its type with ordinary numbers, and a sequence has no
    // single fallback to read a type from. Both keep their own names below.
    template <typename T_Value>
    [[nodiscard]] T_Value get(const std::string& key, T_Value fallback) const;

    // Octal, because a mode written 0660 in a file means what `chmod 660` means and reading it as six
    // hundred and sixty would silently grant something else.
    [[nodiscard]] std::uint32_t getMode(const std::string& key, std::uint32_t fallback) const;

    // Empty when the key is absent. `[]` and a key with no value both give an empty list.
    [[nodiscard]] std::vector<std::string> getList(const std::string& key) const;

    // Named in a refusal so the reader is told which file to fix.
    [[nodiscard]] const std::string& origin() const noexcept
    {
        return origin_;
    }

private:
    // One frame per open map: the column its children must exceed, and the prefix they carry.
    struct Section_t
    {
        std::uint64_t column;
        std::string prefix;
    };

    [[nodiscard]] std::string where(std::uint64_t line) const
    {
        return origin_ + ":" + std::to_string(line) + ": ";
    }

    // nullopt when the path cannot be opened, which carries no values and so cannot mislead. Throws when
    // the file is there and anyone but its owner can write it: what a config decides is what the reader
    // does, so stopping is the only thing the reader can usefully do about that.
    [[nodiscard]] static std::optional<std::string> readIfTrusted(const std::string& path);

    [[nodiscard]] const std::string& require(const std::string& key) const;

    static std::string_view trim(std::string_view text);
    static std::string unquote(std::string_view text);
    static std::uint64_t readIndent(std::string_view line, std::uint64_t lineNumber, const std::string& origin);
    static std::string_view stripComment(std::string_view line);

    // nullopt for a digit outside base 8 or a value above 0777. Apart from getMode(), so the one
    // refusal it turns into is worded once and outside the loop that finds the fault.
    [[nodiscard]] static std::optional<std::uint32_t> readOctal(const std::string& written) noexcept;

    // The three shapes get() dispatches to. Whole is the widest a file can hold, so each caller
    // narrows to its own field and says so in its own refusal.
    [[nodiscard]] std::uint64_t readWhole(const std::string& key, std::uint64_t fallback) const;
    [[nodiscard]] bool readBool(const std::string& key, bool fallback) const;

    std::map<std::string, std::string> entries_;
    std::string origin_;
};

// ── parsing ────────────────────────────────────────────────────────────

inline std::string_view KeyValueConfig::trim(std::string_view text)
{
    constexpr std::string_view Blanks = " \t\r\n";
    const std::uint64_t begin = text.find_first_not_of(Blanks);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(Blanks) - begin + 1);
}

inline std::string KeyValueConfig::unquote(std::string_view text)
{
    const bool quoted = text.size() >= 2 && text.front() == text.back() &&
                        (text.front() == '"' || text.front() == '\'');
    return quoted ? std::string{text.substr(1, text.size() - 2)} : std::string{text};
}

// A '#' opens a comment only at the start of the line or after a blank, which is the YAML rule and
// what lets a value such as `shm:/area#0` keep its own hash. A quoted value is left whole.
inline std::string_view KeyValueConfig::stripComment(std::string_view line)
{
    char quote = '\0';
    for (std::uint64_t index = 0; index < line.size(); ++index)
    {
        const char letter = line[index];
        if (quote != '\0')
        {
            if (letter == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (letter == '"' || letter == '\'')
        {
            quote = letter;
            continue;
        }
        if (letter == '#' && (index == 0 || line[index - 1] == ' ' || line[index - 1] == '\t'))
        {
            return line.substr(0, index);
        }
    }
    return line;
}

// Tabs are refused rather than counted. YAML forbids them in indentation, and one tab counted as
// one column would place a child under the wrong parent without any line looking wrong.
inline std::uint64_t
KeyValueConfig::readIndent(std::string_view line, std::uint64_t lineNumber, const std::string& origin)
{
    const std::uint64_t column = line.find_first_not_of(' ');
    if (column != std::string_view::npos && line[column] == '\t')
    {
        throw ParseError{origin + ":" + std::to_string(lineNumber) + ": tab in indentation"};
    }
    return column == std::string_view::npos ? 0 : column;
}

inline KeyValueConfig KeyValueConfig::parse(std::istream& input, std::string origin)
{
    KeyValueConfig config;
    config.origin_ = std::move(origin);

    std::vector<Section_t> open;
    std::string line;
    std::uint64_t lineNumber = 0;

    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::string_view body = stripComment(line);
        if (trim(body).empty())
        {
            continue;
        }

        const std::uint64_t column = readIndent(body, lineNumber, config.origin_);
        while (!open.empty() && open.back().column >= column)
        {
            open.pop_back();
        }

        const std::uint64_t colon = body.find(':');
        if (colon == std::string_view::npos)
        {
            throw ParseError{config.where(lineNumber) + "not `key: value`"};
        }

        const std::string_view name = trim(body.substr(0, colon));
        if (name.empty())
        {
            throw ParseError{config.where(lineNumber) + "empty key"};
        }

        const std::string path = (open.empty() ? std::string{} : open.back().prefix) + std::string{name};
        const std::string_view value = trim(body.substr(colon + 1));

        // Stored even when empty, and a frame opened either way. A key whose value is blank is a
        // section when deeper lines follow it and an empty setting when they do not, and which one
        // it is only becomes visible on the next line.
        config.entries_[path] = unquote(value);
        if (value.empty())
        {
            open.push_back(Section_t{column, path + "."});
        }
    }

    return config;
}

// An absent file is not untrusted: it carries no values, so there is nothing to be misled by. What is
// refused is a file that exists and that anyone but this process or root can rewrite.
inline std::optional<std::string> KeyValueConfig::readIfTrusted(const std::string& path)
{
    // Closed however this returns, including through the throw below.
    class OpenFile
    {
    public:
        explicit OpenFile(const std::string& named) noexcept
            : descriptor_{::open(named.c_str(), O_RDONLY | O_CLOEXEC)}
        {
        }

        OpenFile(const OpenFile&) = delete;
        OpenFile(OpenFile&&) = delete;
        OpenFile& operator=(const OpenFile&) = delete;
        OpenFile& operator=(OpenFile&&) = delete;

        ~OpenFile() noexcept
        {
            if (descriptor_ >= 0)
            {
                static_cast<void>(::close(descriptor_));
            }
        }

        [[nodiscard]] int get() const noexcept
        {
            return descriptor_;
        }

    private:
        int descriptor_;
    };

    const OpenFile opened{path};
    if (opened.get() < 0)
    {
        return std::nullopt;
    }

    // The descriptor and not the path. A mode read through the path says nothing about the bytes read
    // afterwards, because the name can be pointed at another file in between.
    struct stat found = {};
    if (::fstat(opened.get(), &found) != 0)
    {
        throw ParseError{path + ": cannot be examined"};
    }
    if ((found.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    {
        throw ParseError{path + ": writable by group or other, so it is not trustworthy"};
    }

    // The owner too. A mode this reader cannot fault still lets a third account rewrite the file it
    // owns, so the only writers taken on trust are this process and root.
    if (found.st_uid != ::geteuid() && found.st_uid != 0)
    {
        throw ParseError{path + ": owned by neither this process nor root, so it is not trustworthy"};
    }

    std::string text;
    char buffer[4096];
    while (true)
    {
        const ::ssize_t taken = ::read(opened.get(), buffer, sizeof(buffer));
        if (taken == 0)
        {
            return text;
        }
        if (taken < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw ParseError{path + ": cannot be read"};
        }
        text.append(buffer, static_cast<std::size_t>(taken));
    }
}

inline KeyValueConfig KeyValueConfig::load(const std::string& path)
{
    const std::optional<std::string> text = readIfTrusted(path);
    if (!text)
    {
        throw ParseError{path + ": cannot open"};
    }

    std::istringstream stream{*text};
    return parse(stream, path);
}

inline KeyValueConfig KeyValueConfig::loadIfPresent(const std::string& path)
{
    const std::optional<std::string> text = readIfTrusted(path);
    if (!text)
    {
        KeyValueConfig empty;
        empty.origin_ = path;
        return empty;
    }

    std::istringstream stream{*text};
    return parse(stream, path);
}

// ── typed reads ────────────────────────────────────────────────────────

inline const std::string& KeyValueConfig::require(const std::string& key) const
{
    return entries_.at(key);
}

inline std::uint64_t KeyValueConfig::readWhole(const std::string& key, std::uint64_t fallback) const
{
    if (!has(key) || require(key).empty())
    {
        return fallback;
    }

    const std::string& raw = require(key);
    std::istringstream reader{raw};
    std::uint64_t value = 0;
    reader >> value;
    if (reader.fail() || !reader.eof())
    {
        throw ParseError{origin_ + ": " + key + ": `" + raw + "` is not a number"};
    }
    return value;
}

template <typename T_Value>
inline T_Value KeyValueConfig::get(const std::string& key, T_Value fallback) const
{
    if constexpr (std::is_same_v<T_Value, bool>)
    {
        return readBool(key, fallback);
    }
    else if constexpr (std::is_same_v<T_Value, std::string>)
    {
        return getString(key, fallback);
    }
    else if constexpr (IsDuration<T_Value>)
    {
        // The count, not the unit: the key names the unit and this fallback carries it, so what is
        // read is how many of them.
        using Count = typename T_Value::rep;
        static_assert(std::numeric_limits<Count>::is_integer, "a duration in a file counts whole units");

        const std::uint64_t read = readWhole(key, static_cast<std::uint64_t>(fallback.count()));
        if (read > static_cast<std::uint64_t>(std::numeric_limits<Count>::max()))
        {
            throw ParseError{origin_ + ": " + key + ": `" + require(key) + "` is more of them than fit"};
        }
        return T_Value{static_cast<Count>(read)};
    }
    else
    {
        static_assert(std::is_unsigned_v<T_Value>, "a number in one of these files is never negative");

        const std::uint64_t read = readWhole(key, fallback);
        if (read > std::uint64_t{std::numeric_limits<T_Value>::max()})
        {
            throw ParseError{origin_ + ": " + key + ": `" + require(key) + "` does not fit " +
                             std::to_string(std::numeric_limits<T_Value>::digits) + " bits"};
        }
        return static_cast<T_Value>(read);
    }
}

// Base 8 by hand, since a permission is the one number in these files that is not decimal.
inline std::optional<std::uint32_t> KeyValueConfig::readOctal(const std::string& written) noexcept
{
    std::uint32_t parsed = 0;
    for (const char digit : written)
    {
        if (digit < '0' || digit > '7')
        {
            return std::nullopt;
        }
        parsed = (parsed * 8) + static_cast<std::uint32_t>(digit - '0');
        if (parsed > 0777)
        {
            return std::nullopt;
        }
    }
    return parsed;
}

inline std::uint32_t KeyValueConfig::getMode(const std::string& key, std::uint32_t fallback) const
{
    const std::string written = getString(key, "");
    if (written.empty())
    {
        return fallback;
    }

    const std::optional<std::uint32_t> parsed = readOctal(written);
    if (!parsed)
    {
        throw ParseError{origin_ + ": " + key + ": `" + written + "` is not a permission"};
    }
    return *parsed;
}

inline bool KeyValueConfig::readBool(const std::string& key, bool fallback) const
{
    if (!has(key) || require(key).empty())
    {
        return fallback;
    }

    const std::string& raw = require(key);
    if (raw == "true" || raw == "yes" || raw == "on" || raw == "1")
    {
        return true;
    }
    if (raw == "false" || raw == "no" || raw == "off" || raw == "0")
    {
        return false;
    }
    throw ParseError{origin_ + ": " + key + ": `" + raw + "` is not a boolean"};
}

inline std::vector<std::string> KeyValueConfig::getList(const std::string& key) const
{
    std::vector<std::string> items;
    if (!has(key))
    {
        return items;
    }

    std::string_view raw = require(key);
    if (raw.empty())
    {
        return items;
    }
    if (raw.front() != '[' || raw.back() != ']')
    {
        throw ParseError{origin_ + ": " + key + ": `" + std::string{raw} + "` is not an inline sequence"};
    }

    raw = raw.substr(1, raw.size() - 2);
    while (!trim(raw).empty())
    {
        const std::uint64_t comma = raw.find(',');
        items.push_back(unquote(trim(raw.substr(0, comma))));
        if (comma == std::string_view::npos)
        {
            break;
        }
        raw = raw.substr(comma + 1);
    }
    return items;
}

}  // namespace kvconfig
