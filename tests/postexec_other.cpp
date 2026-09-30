// SPDX-License-Identifier: Apache-2.0
//
// postexec_other -- the image test_postexec_attack execs into, and nothing more.
//
// A separate executable rather than a second mode of that case: the delegation row binds an inode,
// which a self re-exec keeps. The execve generation is what changes instead, and
// test_exec_generation checks that.
//
// Everything arrives on the command line, since execve leaves this image no descriptor it did not
// open itself. It exits with the errno the kernel gave, or 0 when the mapping landed, which is the
// same convention the forked children in the suite use.
//
//   postexec_other <mount> <name>

#include <cstdio>

#include "harness/harness.hpp"
#include "harness/probe.hpp"

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <mount> <name>\n", argv[0]);
        return 2;
    }

    return fsuser::tests::openAndProbeReadMap(argv[1], argv[2], fsuser::tests::PlacementSize);
}
