<!-- CONTRIBUTING.md has the style rules and the gates. Small PRs need little here. -->

## What and why

<!-- What changed, and what problem it solves. Link an issue if there is one. -->

## Testing

<!-- Which gates you ran and what they said. "./check.sh: 0 gates failed, suite on a devdax host" is checkable, and "works for me" is not. -->

## Checklist

- [ ] `./check.sh --gate format` leaves the tree unchanged.
- [ ] The gates that need no device pass: headers, docs, build, tidy and daemon.
- [ ] The suite passes on a devdax host, if this reaches the kernel module, the daemon's answers, or what a mount does.
- [ ] `docs/design.md` matches, if the layout, the upcall protocol or the permission model changed.
