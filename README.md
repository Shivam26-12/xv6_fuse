# Project 10, Part 2 — xv6 FUSE with Hierarchical Inode-Based Filesystem

> **Upgrade over the original flat-file design**: the daemon now manages a full
> in-memory inode tree so you get real directories (`mkdir`), nested paths, and
> unlink — just like a real filesystem.

---

## 1. Architecture

```
  cat /fuse/subdir/file                            fused (user daemon)
       |                                                ^      |
       | open/read                                      |read  |write
       v                                                |      v
 +----------- xv6 kernel -------------------------------------------------+
 | sys_open  ---> fuse_route("/fuse/subdir/file") -> FUSEDEV              |
 | sys_mkdir ---> fuse_route("/fuse/newdir")      -> fuse_do_mkdir()      |
 | sys_unlink --> fuse_route("/fuse/file")         -> fuse_do_unlink()    |
 |    |                                                                    |
 |    v                                                                    |
 | struct file { type = FD_FUSE, fuse_path[128], fuse_fh, fuse_type }     |
 |    | fileread/filewrite/filestat/fileclose dispatch to fuse.c           |
 |    v                                                                    |
 | slot table (8 slots) FREE -> FILL -> PENDING -> INFLIGHT -> DONE        |
 |    caller: sleep(slot)          daemon: read(/dev/fuse) pops oldest     |
 |    wakeup(slot) <-------------- write(/dev/fuse) delivers reply         |
 +-------------------------------------------------------------------------+

               fused (user/fused.c) — in-memory inode tree
  ┌─────────────────────────────────────────────────────────┐
  │  inode[0]  ino=1  T_DIR  name="/"   children=[2,3,...]  │  root
  │  inode[1]  ino=2  T_FILE name="hello" data=...           │
  │  inode[2]  ino=3  T_DIR  name="subdir" children=[4,5]   │
  │  inode[3]  ino=4  T_FILE name="file1"  data=...          │
  │  inode[4]  ino=5  T_DIR  name="nested" children=[6]     │
  │  inode[5]  ino=6  T_FILE name="deep"   data=...          │
  └─────────────────────────────────────────────────────────┘
```

| Piece | Where | What it does |
|---|---|---|
| Mount table | `kernel/fuse.c` (`mtab[]`, `fuse_route`) | Maps `/fuse` prefix to FUSEDEV. Called from `sys_open`, `sys_mkdir`, `sys_unlink`. |
| `/dev/fuse` | `kernel/fuse.c` (`devsw[FUSEDEV]`) | Daemon `read` = next request, daemon `write` = reply. |
| Request queue | `kernel/fuse.c` | 8 slots, one spinlock, sleep/wakeup. |
| Protocol | `kernel/fuse.h` | `struct fuse_req` / `struct fuse_rep`; ops: OPEN, READ, WRITE, READDIR, GETATTR, RELEASE, **MKDIR**, **UNLINK**, **RMDIR**. |
| File dispatch | `kernel/file.c`, `file.h` | File type `FD_FUSE`; `fuse_path` enlarged to 128 bytes. |
| Routing hook | `kernel/sysfile.c` | `sys_open`, **`sys_mkdir`**, **`sys_unlink`** each check the mount table first. |
| Daemon | `user/fused.c` | Hierarchical inode tree; 128 inodes, up to 32 children per dir, 4 KiB per file. |
| Tests | `user/fusetest.c` | All original tests **plus** nested mkdir, files in subdirs, deeply nested paths, concurrent per-process subdirectories. |

---

## 2. What's New vs. the Original Flat Design

| Feature | Original | **Upgraded** |
|---|---|---|
| Directory structure | Flat (only `/fuse/*.ext`) | **Hierarchical tree** (`/fuse/a/b/c`) |
| `mkdir` support | ❌ | ✅ via `FUSE_MKDIR` op + `sys_mkdir` hook |
| `unlink` support | ❌ | ✅ via `FUSE_UNLINK` op + `sys_unlink` hook |
| `rmdir` support | ❌ | ✅ via `FUSE_RMDIR` op |
| Inode numbering | Sequential handle (1-based index) | **True inode IDs** (tree-rooted at ino=1) |
| Path resolution | Flat name split at `/` | **Full recursive walk** (`path_lookup`) |
| Capacity | 16 files, 4 KiB each | 128 inodes (files+dirs), 4 KiB per file, 32 children/dir |
| `FUSE_PATHMAX` | 64 | **128** (supports deeper paths) |

---

## 3. Protocol (new ops)

| op | request fields | reply fields |
|---|---|---|
| `FUSE_OPEN` | path, flags | err, type (`T_FILE`/`T_DIR`), fh |
| `FUSE_READ` | path, fh, off, len | err, len, data |
| `FUSE_WRITE` | path, fh, off, len, data | err, len (bytes accepted) |
| `FUSE_READDIR` | path, off, len | err, len, data = packed `struct dirent[]` |
| `FUSE_GETATTR` | path, fh | err, type, size |
| `FUSE_RELEASE` | path, fh | *(no reply)* |
| **`FUSE_MKDIR`** | path | err |
| **`FUSE_UNLINK`** | path | err |
| **`FUSE_RMDIR`** | path | err |

---

## 4. Install (≈ 2 minutes)

Requires the xv6-riscv tree (`~/xv6-riscv`) and Python 3.

```bash
cd Project10_Part2_xv6_FUSE
bash install.sh ~/xv6-riscv
cd ~/xv6-riscv
make clean && make qemu
```

If a patch step reports `FAIL`, apply it manually (see §7 below).

---

## 5. Demo script (inside xv6)

### Hello (read-only) mode
```
$ fused hello -v &
fused: serving /fuse (hello, read-only)
$ cat /fuse/hello
Hello from the xv6 FUSE daemon!
$ fusetest
fusetest: ALL PASSED
```

### Hierarchical read-write mode
```
$ fused mem -v &
fused: serving /fuse (in-memory hierarchical, read-write)

# Create a directory tree
$ mkdir /fuse/docs
$ mkdir /fuse/src
$ mkdir /fuse/src/lib

# Write files anywhere in the tree
$ echo "hello world" > /fuse/docs/readme
$ echo "int main(){}" > /fuse/src/main.c
$ echo "void foo(){}" > /fuse/src/lib/util.c

# Browse the tree
$ ls /fuse
.  ..  hello  docs  src
$ ls /fuse/src
.  ..  main.c  lib
$ ls /fuse/src/lib
.  ..  util.c

$ cat /fuse/docs/readme
hello world

# Run the full test suite
$ fusetest mem
fusetest: ALL PASSED
```

---

## 6. What `fusetest` checks

**Hello mode:**
read contents, `fstat` type/size, EOF returns 0, missing file fails, disk paths unaffected, `/fuse` is a directory, `readdir` lists hello, write refused.

**Mem (hierarchical) mode** *(everything above plus)*:
- 3000-byte write in multiple chunks, O_TRUNC rewrite
- `mkdir /fuse/subdir` → appears in `readdir`
- file created inside subdir, read back byte-for-byte
- nested dir `/fuse/subdir/nested` created and listed
- file at depth-3 path `/fuse/subdir/nested/deep`
- multiple dirs at root: `docs`, `src`; files inside each
- **4 concurrent processes** each creating their own `/fuse/procN/f` subdir + file, writing and verifying without interference

---

## 7. Manual edits (only if `install.sh` reports FAIL)

| File | What to add |
|---|---|
| `kernel/file.h` | Add `FD_FUSE` to the `enum`; add `uint fuse_fh; short fuse_type; char fuse_path[128];` to `struct file`. |
| `kernel/file.c` | `fileinit()` calls `fuseinit();`. `fileclose`: add `FD_FUSE` branch calling `fuse_release`. `filestat`: add `FD_FUSE` branch calling `fuse_getattr`. `fileread`/`filewrite`: add `FD_FUSE` branches. |
| `kernel/defs.h` | Add prototypes: `fuseinit`, `fuse_attach`, `fuse_route`, `fuse_do_open`, `fuse_do_mkdir`, `fuse_do_unlink`, `fuse_read`, `fuse_write`, `fuse_getattr`, `fuse_release`, `fuse_proc_exit`. |
| `kernel/sysfile.c` | `#include "fuse.h"`. In `sys_open`, after `argstr`, add `fuse_route` / `fuse_do_open` block. In device-open branch add `if(ip->major == FUSEDEV) fuse_attach();`. In `sys_mkdir`, before `begin_op()`, add `fuse_route` / `fuse_do_mkdir` block. In `sys_unlink`, before `begin_op()`, add `fuse_route` / `fuse_do_unlink` block. |
| `kernel/proc.c` | In `exit()`, after `p->cwd = 0;` add `fuse_proc_exit(p->pid);`. |
| `Makefile` | Add `$K/fuse.o` to `OBJS`; add `$U/_fused` and `$U/_fusetest` to `UPROGS`. |

---

## 8. Limitations

- Only absolute paths under `/fuse` are routed (no per-process mount namespaces).
- Daemon is single-threaded; requests are handled one at a time (kernel queue holds 8 waiting callers).
- One daemon; whoever first opens `/dev/fuse` becomes the daemon.
- `mem` backend: 128 inodes, 32 children per directory, 4 KiB per file, names up to 14 characters.
- No persistence: everything lives in the daemon's memory and vanishes when it exits.
- No hard links, symlinks, or `rename`.
