# libroof

A small library over the syscalls an application makes on a mount of this filesystem.
It holds the descriptor and the mapping for the caller, 
closing and unmapping on the way out, 
and sends the permission ioctls without the caller naming the request record.
The contract the kernel holds every caller to, library or not, is in [`kernel/`](../kernel/).

## The verbs

| Verb | Syscall | What the caller gets |
|---|---|---|
| `File::open(path, flags, mode)` | `open` | A descriptor with `O_CLOEXEC` added. With `O_CREAT` the region has a name and a slot, and no bytes yet. |
| `File::resize(bytes)` | `ftruncate` | The region's physical extent, rounded up to `DaxAlignment`. It is placed once: a second `resize` on a placed file is refused. |
| `File::map(bytes, protection)` | `mmap` | A `Mapping` that unmaps on the way out. A caller who is not the owner gets a delegation row written for it on its first map, if the daemon allows. |
| `File::setCachePolicy(policy)` | `ioctl` | Which pool the placement takes, and so how every later mapping is cached. Accepted only while the file has no extent. |
| `File::readCachePolicy()` | `ioctl` | Where the region landed, or what `setCachePolicy` asked for while it has no extent. |
| `File::setDefaultPermission(perms)` | `ioctl` | What the region grants to everyone without a row of their own. Needs `ADMIN`. |
| `File::grantPermission(uid, gid, perms)` | `ioctl` | A delegation row for an account on this node. `AnyId` in one id leaves the other to decide. |
| `File::revokePermission(uid, gid)` | `ioctl` | That row taken off. A row naming the caller needs nothing, any other needs `ADMIN`, and a row belongs to the node that wrote it. A gid reaches a process row by the groups its holder carries, since the row carries only the one it ran under. |
| `unlinkFile(path)` | `unlink` | The region gone once the last descriptor closes. Only the creating node may remove it, so a peer's file answers `EPERM`. |

The perm verbs take a `Permission`, a mask built by or'ing `Permission::Read`, `Write`, `Delete`, `Admin`, `Ioctl` and `Grant`, and the cache verbs take a `CachePolicy`, `Writeback` or `Uncached`, so a caller needs no kernel header.
A `File` built from a descriptor adopts it 
and closes it on the way out, 
and `get()` hands the descriptor back for a syscall the library does not wrap.

Every verb throws rather than returns a code.
`FsCodedError` carries the errno the syscall left, 
which `code()` hands back for comparison against `std::errc`.

## Example

```cpp
#include "roof/errors.hpp"
#include "roof/file.hpp"

using roof::CachePolicy;
using roof::Permission;

auto file = roof::File::open("/mnt/rooffs/weights", O_CREAT | O_RDWR, 0644);
// The pool is chosen before the bytes exist.
file.setCachePolicy(CachePolicy::Uncached);
CachePolicy policy = file.readCachePolicy();

file.resize(roof::DaxAlignment);

// Unmapped when `mapped` goes out of scope.
auto mapped = file.map(roof::DaxAlignment, PROT_READ | PROT_WRITE);

// What everyone without a row of their own gets.
file.setDefaultPermission(Permission::Read);

// A row for one account on this node, named by uid alone.
constexpr uid_t consumer = 1001;
file.grantPermission(consumer, roof::AnyId, Permission::Read | Permission::Write);
file.revokePermission(consumer, roof::AnyId);

// The descriptor, for a syscall the library does not wrap.
::fsync(file.get());

roof::unlinkFile("/mnt/rooffs/weights");
```
<!-- fsname -->

A refused verb throws `FsCodedError`, and `code()` is the errno as a `std::error_code`.
