// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// util/text.hpp -- the string shaping more than one part of the daemon does.
//
// A config line, a rules line, a /proc status row, a SPIFFE path and an audit field are each cut or
// cleaned the same way, so the cut lives once here rather than beside each reader.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fsdaemon::util
{

// @text without leading or trailing blanks. The carriage return counts as trailing, because a file
// written on another host arrives with one.
[[nodiscard]] inline std::string_view trimSpace(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

// Whether @text opens with @prefix.
[[nodiscard]] inline bool hasPrefix(std::string_view text, std::string_view prefix) noexcept
{
    return text.substr(0, prefix.size()) == prefix;
}

// The non-empty runs of @text separated by any character of @delimiters. The views point into
// @text, so it has to outlive them.
[[nodiscard]] inline std::vector<std::string_view> splitFields(std::string_view text,
                                                               std::string_view delimiters)
{
    std::vector<std::string_view> tokens;
    std::uint64_t start = text.find_first_not_of(delimiters);
    while (start != std::string_view::npos)
    {
        const std::uint64_t stop = text.find_first_of(delimiters, start);
        tokens.push_back(text.substr(start, stop - start));
        start = text.find_first_not_of(delimiters, stop);
    }
    return tokens;
}

// @text with every control character turned into a space, so a value carrying a newline cannot
// split one record into two lines.
[[nodiscard]] inline std::string flatten(std::string_view text)
{
    std::string out{text};
    for (auto& here : out)
    {
        if (static_cast<std::uint8_t>(here) < ' ')
        {
            here = ' ';
        }
    }
    return out;
}

}  // namespace fsdaemon::util
