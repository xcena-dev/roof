// SPDX-License-Identifier: Apache-2.0
//
// test_mount_table -- which node id the helpers read off a mount table line.
//
// Both helpers name the daemon they start and stop after this id, so a line that claimed no node
// has to read as no id rather than as node 0.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>

#include "mount_table.hpp"
#include "name.h"

namespace
{

std::int32_t g_failureCount = 0;

void check(const std::string& label, bool passed)
{
    std::printf("%s: %s\n", passed ? "PASS" : "FAIL", label.c_str());
    if (!passed)
    {
        ++g_failureCount;
    }
}

// The id findNodeId answers for /mnt/x when @table is the whole mount table.
[[nodiscard]] std::optional<std::uint32_t> findIn(const std::string& table)
{
    std::istringstream mounts{table};
    return fstools::findNodeId(mounts, "/mnt/x");
}

// One line of this filesystem at /mnt/x carrying @options.
[[nodiscard]] std::string makeLine(const std::string& options)
{
    return std::string{FS_NAME_STR " /mnt/x " FS_NAME_STR " "}.append(options).append(" 0 0\n");
}

}  // namespace

int main()
{
    check("a claimed id reads as itself", findIn(makeLine("rw,relatime,node_id=3,daemon_uid=4242")) == 3U);
    check("an id of 0 is a claim that did not land", !findIn(makeLine("rw,node_id=0")).has_value());
    check("an option that only ends in node_id= is not the id",
          findIn(makeLine("rw,xnode_id=5,node_id=7")) == 7U);
    check("another filesystem at the same path names no node",
          !findIn("tmpfs /mnt/x tmpfs rw,node_id=3 0 0\n").has_value());
    check("a mount at another path names no node for this one",
          !findIn(FS_NAME_STR " /mnt/y " FS_NAME_STR " rw,node_id=3 0 0\n").has_value());

    std::printf("%d check(s) failed\n", g_failureCount);
    return g_failureCount == 0 ? 0 : 1;
}
