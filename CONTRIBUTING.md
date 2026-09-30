# Contributing

Any contribution is welcome.
A typo fix, a question that turns out to be a documentation gap, a measurement that contradicts ours, a bug report with no patch attached:
all of it helps,
and none of it needs to be large.

If you are unsure whether something is worth raising, raise it.

## Where things go

Questions and design discussion belong in [Discussions](../../discussions).
Bugs and concrete proposals belong in [Issues](../../issues).
A suspected vulnerability goes to a [private advisory](../../security/advisories/new),
not to a public issue.

For a big change, an issue first saves you a rewrite,
because the layout in fabric-attached memory, the upcall protocol and the permission model are all still moving.
For anything small, just open the pull request.

## Style

Two tools decide it, so there is nothing to memorise:

```sh
./check.sh --gate format   # clang-format-18 over the userspace and the kernel module
./check.sh --gate tidy     # clang-tidy-18 from .clang-tidy
./check.sh --fix           # let clang-format rewrite what it would have complained about
```

`clang-format` is pinned to 18,
because its output changes between major releases.
The kernel module has its own `kernel/.clang-format`,
and the formatter picks the config from each file's path.

The one rule that changes how you write rather than how it formats:
every identifier is at least 3 characters.
`nodeId`, not `id`.
`file`, not `fd`.

Comments say what the code does now, never what it used to do.
That belongs in the commit message.

## Testing

`check.sh` runs every gate in one pass,
and its exit code is the number of gates that failed.

```sh
./check.sh               # every gate
./check.sh --gate build  # one gate by name
```

Six of the gates need no special hardware:
headers, docs, build, format, tidy and daemon.
The suite needs a CXL devdax device with two mounts of it,
which `tools/deploy/install.sh` sets up with two `--mount` options.
Run it too if your change reaches the kernel module, the daemon's answers, or what a mount does.
Say in the pull request which gates you ran.

If you change the layout in fabric-attached memory, the upcall protocol or the permission model,
update [`docs/design.md`](docs/design.md) with it.
If the change moves an attack scenario, update [`docs/security_report.md`](docs/security_report.md) as well.

## Licence

Apache-2.0, as in [`LICENSE`](LICENSE),
except `kernel/`, which is GPL-2.0-only as in [`kernel/LICENSE`](kernel/LICENSE).
Keep the SPDX line on new files.
