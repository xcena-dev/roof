// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// util/file_text.hpp -- a whole file as one string.
//
// The identity rules and the rego policy are both a file this daemon reads end to end and then
// parses. They disagree about what an unreadable one means, so this answers nullopt and each caller
// decides.

#pragma once

#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>

namespace fsdaemon::util
{

// The whole of the file at @path, or nullopt when it cannot be opened.
[[nodiscard]] inline std::optional<std::string> readFileText(const std::string& path)
{
    const std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace fsdaemon::util
