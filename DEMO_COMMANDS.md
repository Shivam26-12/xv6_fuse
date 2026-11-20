# xv6 Hierarchical FUSE — Complete Startup & Demo Guide

This file contains the exact sequence of commands to boot xv6, run the demonstration for your instructor, and execute the automated test suite.

---

## Part 1: Starting the Project from WSL

Open your WSL terminal and run:

```bash
cd ~/xv6-riscv
make clean && make qemu
```

Wait a few seconds for xv6 to boot until you see the shell prompt:
```text
xv6 kernel is booting

hart 1 starting
hart 2 starting
init: starting sh
$ 
```

---

## Part 2: Instructor Live Demo (Clean Output Mode)

> **Tip:** We start the daemon **without `-v`** here so the output is clean and formatted into neat columns without background log messages cutting into text.

### Step 1: Start the FUSE Inode Daemon
Type:
```bash
fused mem &
```
*(Wait 1 second until you see: `fused: in-memory hierarchical fs (read-write, inode-based)`)*

---

### Step 2: Show Hierarchical Directory Creation (`mkdir`)
```bash
mkdir /fuse/docs
mkdir /fuse/src
mkdir /fuse/src/lib
```
*(Commands finish silently with return code 0, proving successful creation)*

---

### Step 3: Inspect the Inode Hierarchy (`ls`)
```bash
ls /fuse
```
**Output Explanation:**
```text
.              1 1 64      <-- . (current dir), Type 1 (T_DIR), Inode 1 (Root), Size 64 bytes
..             1 1 64      <-- .. (parent dir)
hello          2 2 32      <-- hello, Type 2 (T_FILE), Inode 2, Size 32 bytes
docs           1 3 32      <-- docs folder, Type 1 (T_DIR), Inode 3
src            1 4 48      <-- src folder, Type 1 (T_DIR), Inode 4
```

Now list the nested subdirectory:
```bash
ls /fuse/src
```
**Output Explanation:**
```text
.              1 4 48      <-- Inode 4 (src)
..             1 1 64      <-- Inode 1 (parent is root)
lib            1 5 32      <-- Inode 5 (lib, nested directory!)
```

---

### Step 4: Write and Read Deeply Nested Files
Create and read files inside different directories in the tree:

```bash
# 1. Write file in /fuse/docs/
echo hello-os > /fuse/docs/readme
cat /fuse/docs/readme

# 2. Write file 3 levels deep in /fuse/src/lib/
echo my-code > /fuse/src/lib/main.c
cat /fuse/src/lib/main.c
```

Now inspect the directory contents to show the newly created files:
```bash
ls /fuse/docs
ls /fuse/src/lib
```

---

### Step 5: Read the Default Hello File
```bash
cat /fuse/hello
```
Expected output:
```text
Hello from the xv6 FUSE daemon!
```

---

## Part 3: Automated Test Suite (100% PASS)

Run the full automated test suite (tests 3,000-byte streaming, nested directories, file truncation, and 4 concurrent processes):

```bash
fusetest mem
```

**Expected Final Output:**
```text
...
ok:   create + write 3000 bytes
ok:   read back 3000 bytes, contents identical
ok:   mkdir /fuse/subdir succeeds
ok:   /fuse/subdir is a directory
ok:   mkdir /fuse/subdir/nested succeeds
ok:   /fuse/subdir/nested is a directory
ok:   4 concurrent processes each create a subdir and write/verify files
ok:   concurrent subdirectories proc0..proc3 all present
fusetest: ALL PASSED
```

---

## Part 4: Optional — Showing IPC Packet Tracing to Instructor (`-v` Verbose Mode)

If your instructor wants to see the **kernel-to-daemon request packets** in real time:

1. Exit xv6 (`Ctrl-A` then `X`).
2. Run `make qemu`.
3. Start the daemon with `-v`:
   ```bash
   fused mem -v &
   ```
4. Now run any command (e.g., `mkdir /fuse/testdir` or `cat /fuse/hello`).
5. Point out to your instructor:
   - `#ID`: The kernel ticket number matching requests and replies.
   - `OP`: The filesystem action (`MKDIR`, `OPEN`, `READ`, `WRITE`, `GETATTR`, `READDIR`).
   - `off=0 len=0 -> ok`: How payload and offsets are transferred across the `/dev/fuse` character device.

---

## Part 5: Exiting xv6

To cleanly exit xv6 back to your WSL terminal prompt:
- Press **`Ctrl` + `A`**, release both keys, then press **`X`**.
