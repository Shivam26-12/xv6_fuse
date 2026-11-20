#!/bin/bash
# Usage: ./install.sh [path-to-xv6-riscv]     (default: ~/xv6-riscv)
# Copies the new files in and patches the existing xv6 sources.
# Originals are saved as <file>.orig.  Safe to run twice.
#
# UPGRADED: Hierarchical inode-based FUSE with mkdir support.
# AUTO-DETECTS sleep/copyout/copyin signatures from your xv6 version.
set -e
ROOT="${1:-$HOME/xv6-riscv}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$ROOT/kernel/file.c" ] || { echo "Not an xv6-riscv tree: $ROOT"; exit 1; }

echo "== copying new files into $ROOT"
cp "$HERE/kernel/fuse.c" "$HERE/kernel/fuse.h" "$ROOT/kernel/"
cp "$HERE/user/fused.c" "$HERE/user/fusetest.c" "$ROOT/user/"

echo "== patching existing files"
python3 - "$ROOT" <<'PY'
import re, os, shutil, sys
root = sys.argv[1]
bad = 0

def patch(path, rx, fn, marker):
    global bad
    full = os.path.join(root, path)
    s = open(full).read()
    if marker in s:
        print("  skip  %-20s already has %s" % (path, marker)); return
    new, n = re.subn(rx, fn, s, count=1)
    if n != 1:
        print("  FAIL  %-20s anchor not found: %s" % (path, rx)); bad += 1; return
    if not os.path.exists(full + ".orig"):
        shutil.copy(full, full + ".orig")
    open(full, "w").write(new)
    print("  ok    %-20s %s" % (path, marker))

# -------------------------------------------------------------------------
# AUTO-DETECT: Check actual sleep signature in proc.c
# -------------------------------------------------------------------------
proc_src = open(os.path.join(root, "kernel/proc.c")).read()
vm_src   = open(os.path.join(root, "kernel/vm.c")).read()
defs_src = open(os.path.join(root, "kernel/defs.h")).read()

# Detect sleep signature: look for the actual definition in proc.c
if re.search(r'^void\s+sleep\s*\(\s*void\s*\*', proc_src, re.MULTILINE):
    SLEEP_SIG = "void*, struct spinlock*"
    print("  detect sleep(void*, struct spinlock*) [standard]")
elif re.search(r'^void\s+ksleep\s*\(\s*void\s*\*', proc_src, re.MULTILINE):
    SLEEP_SIG = "ksleep"
    print("  detect ksleep() variant")
else:
    SLEEP_SIG = "void*, struct spinlock*"
    print("  detect sleep(void*, struct spinlock*) [assumed standard]")

# Detect copyout arg count from vm.c
m = re.search(r'int\s+copyout\s*\(([^)]+)\)', vm_src)
if m:
    args = [a.strip() for a in m.group(1).split(',')]
    COPYOUT_NARGS = len(args)
    print("  detect copyout() has %d args" % COPYOUT_NARGS)
else:
    COPYOUT_NARGS = 4
    print("  detect copyout() has 4 args [assumed standard]")

# ---- kernel/file.h : new file type + per-file state (fuse_path enlarged to 128)
patch("kernel/file.h", r"FD_DEVICE\s*\}\s*type;",
      lambda m: "FD_DEVICE, FD_FUSE } type;", "FD_FUSE")
patch("kernel/file.h", r"short major;[^\n]*\n",
      lambda m: m.group(0) +
      "  // FD_FUSE (see fuse.c)\n  uint fuse_fh;\n  short fuse_type;\n  char fuse_path[128];  // must equal FUSE_PATHMAX\n",
      "fuse_fh")

# ---- kernel/file.c : dispatch the file operations
patch("kernel/file.c", r'initlock\(&ftable\.lock, "ftable"\);',
      lambda m: m.group(0) + "\n  fuseinit();", "fuseinit();")
patch("kernel/file.c", r"else if\(ff\.type == FD_INODE \|\| ff\.type == FD_DEVICE\)\{",
      lambda m: "else if(ff.type == FD_FUSE){\n    fuse_release(&ff);\n  } " + m.group(0),
      "fuse_release")
patch("kernel/file.c", r"if\(f->type == FD_INODE \|\| f->type == FD_DEVICE\)\{",
      lambda m: ("if(f->type == FD_FUSE){\n    struct stat fst;\n    if(fuse_getattr(f, &fst) < 0)\n      return -1;\n"
                 "    if(copyout(p->pagetable, addr, (char *)&fst, sizeof(fst)) < 0)\n      return -1;\n    return 0;\n  }\n  ")
                + m.group(0), "fuse_getattr")
patch("kernel/file.c", r'else\s*\{\s*panic\("fileread"\);',
      lambda m: "else if(f->type == FD_FUSE){\n    r = fuse_read(f, addr, n);\n  } " + m.group(0), "fuse_read")
patch("kernel/file.c", r'else\s*\{\s*panic\("filewrite"\);',
      lambda m: "else if(f->type == FD_FUSE){\n    ret = fuse_write(f, addr, n);\n  } " + m.group(0), "fuse_write")

# ---- kernel/defs.h : prototypes (appended, with correct sleep sig)
full = os.path.join(root, "kernel/defs.h")
s = open(full).read()
if "fuse_route" in s:
    print("  skip  kernel/defs.h       already has fuse_route")
else:
    if not os.path.exists(full + ".orig"): shutil.copy(full, full + ".orig")
    s += """
// fuse.c
void            fuseinit(void);
void            fuse_attach(void);
int             fuse_route(char*, char*);
int             fuse_do_open(char*, int, int*, int*);
int             fuse_do_mkdir(char*);
int             fuse_do_unlink(char*);
int             fuse_read(struct file*, uint64, int);
int             fuse_write(struct file*, uint64, int);
int             fuse_getattr(struct file*, struct stat*);
void            fuse_release(struct file*);
void            fuse_proc_exit(int);
"""
    open(full, "w").write(s)
    print("  ok    kernel/defs.h       prototypes")

# ---- PATCH fuse.c to fix sleep/copyout/copyin for this xv6 version ----
# After copying fuse.c to $ROOT/kernel/, we may need to patch it.
fuse_full = os.path.join(root, "kernel/fuse.c")
fuse_src  = open(fuse_full).read()
fuse_modified = False

# Fix 1: If defs.h declares sleep with wrong sig, the POSIX sleep(unsigned)
# was probably declared. We work around this by using the full proc.h
# sleep path. The simplest fix: define a macro to alias sleep.
#
# Check whether defs.h has 'void sleep(void)' (bad) or '(void*, struct spinlock*)' (good)
defs_now = open(os.path.join(root, "kernel/defs.h")).read()
m_sleep = re.search(r'void\s+sleep\s*\(([^)]*)\)', defs_now)
if m_sleep:
    sleep_args = m_sleep.group(1).strip()
    if sleep_args == 'void' or sleep_args == '':
        print("  warn  defs.h has bad sleep(void) - will fix it")
        # Remove the bad declaration and add the correct one
        new_defs = re.sub(r'void\s+sleep\s*\(\s*void\s*\)\s*;', 
                          'void            sleep(void*, struct spinlock*);', defs_now, count=1)
        open(os.path.join(root, "kernel/defs.h"), "w").write(new_defs)
        print("  ok    defs.h: fixed sleep signature to (void*, struct spinlock*)")
    elif 'spinlock' in sleep_args:
        print("  ok    defs.h: sleep signature is correct")
    else:
        print("  warn  defs.h: unusual sleep sig: %s" % sleep_args)

# Fix 2: copyout/copyin argument count adaptation in fuse.c
# Our fuse.c uses either_copyout / either_copyin (not copyout/copyin directly)
# so the 5-arg variant shouldn't affect fuse.c - but filestat in file.c uses copyout.
# We need to fix the copyout in the filestat patch to match the actual signature.
file_c_full = os.path.join(root, "kernel/file.c")
file_c_src  = open(file_c_full).read()

# Check actual copyout signature in vm.c
vm_match = re.search(r'int\s+copyout\s*\(([^)]+)\)', vm_src)
if vm_match:
    vm_args = vm_match.group(1)
    # Count commas = number of args - 1
    n_co = vm_args.count(',') + 1
    if n_co == 5 and "fuse_getattr" in file_c_src:
        # Need to change copyout(p->pagetable, addr, (char*)&fst, sizeof(fst))
        # to copyout(p->pagetable, 0, addr, (char*)&fst, sizeof(fst))  
        # We don't know what the 2nd arg is exactly; it might be a flags/perm arg.
        # Look at how other places in file.c call copyout:
        ex = re.search(r'copyout\(([^)]+)\)', file_c_src)
        if ex:
            ex_args = [a.strip() for a in ex.group(1).split(',')]
            if len(ex_args) == n_co:
                print("  ok    file.c: copyout has %d args, existing calls match" % n_co)
            else:
                print("  warn  file.c: copyout arg mismatch, checking fuse_getattr patch...")
                # Try to find a working copyout call in file.c to use as template
                # Look for the filestat block we inserted
                if "fuse_getattr" in file_c_src:
                    # Fix our inserted copyout call
                    # Find what the 2nd extra arg should be from other copyout calls
                    all_co = re.findall(r'copyout\(([^)]+)\)', file_c_src)
                    working = None
                    for co in all_co:
                        co_args = [a.strip() for a in co.split(',')]
                        if len(co_args) == n_co and 'fst' not in co:
                            working = co_args
                            break
                    if working:
                        # Reconstruct the copyout with correct args
                        # working[0]=pagetable, working[1]=extra, working[2]=addr, working[3]=src, working[4]=len
                        extra_arg = working[1]
                        old_co = r'copyout\(p->pagetable, addr, \(char \*\)&fst, sizeof\(fst\)\)'
                        new_co = 'copyout(p->pagetable, %s, addr, (char *)&fst, sizeof(fst))' % extra_arg
                        new_file_c, n_rep = re.subn(old_co, new_co, file_c_src, count=1)
                        if n_rep:
                            open(file_c_full, "w").write(new_file_c)
                            print("  ok    file.c: fixed 5-arg copyout in fuse_getattr block")

print("")

# ---- kernel/sysfile.c : routing in sys_open + attach on /dev/fuse
patch("kernel/sysfile.c", r'#include "fcntl.h"\n',
      lambda m: m.group(0) + '#include "fuse.h"\n', '"fuse.h"')

# Hook into sys_open (file open routing)
OPEN_HOOK = """
  {
    // paths under a mount point (e.g. /fuse) are served by a user daemon
    char rel[FUSE_PATHMAX];
    int rr = fuse_route(path, rel);
    if(rr == -2)
      return -1;
    if(rr == 0){
      struct file *ff;
      int ffd = -1, ftype = 0, ffh = 0;
      if((ff = filealloc()) == 0)
        return -1;
      if(fuse_do_open(rel, omode, &ftype, &ffh) < 0){
        fileclose(ff);
        return -1;
      }
      ff->type = FD_FUSE;
      ff->off = 0;
      ff->readable = !(omode & O_WRONLY);
      ff->writable = (omode & O_WRONLY) || (omode & O_RDWR);
      ff->fuse_fh = ffh;
      ff->fuse_type = ftype;
      safestrcpy(ff->fuse_path, rel, FUSE_PATHMAX);
      if((ftype == T_DIR && omode != O_RDONLY) || (ffd = fdalloc(ff)) < 0){
        fileclose(ff);
        return -1;
      }
      return ffd;
    }
  }
"""
patch("kernel/sysfile.c", r"\(n = argstr\(0, path, MAXPATH\)\) < 0[^\n]*\n\s*return -1;\n",
      lambda m: m.group(0) + OPEN_HOOK, "fuse_route")
patch("kernel/sysfile.c", r"f->major = ip->major;",
      lambda m: m.group(0) + "\n    if(ip->major == FUSEDEV)\n      fuse_attach();", "fuse_attach")

# Hook into sys_mkdir
MKDIR_HOOK = """
  {
    char rel[FUSE_PATHMAX];
    int rr = fuse_route(path, rel);
    if(rr == -2)
      return -1;
    if(rr == 0)
      return fuse_do_mkdir(rel);
  }
"""
full_sf = os.path.join(root, "kernel/sysfile.c")
sf = open(full_sf).read()
if "fuse_do_mkdir" not in sf:
    new_sf, cnt = re.subn(
        r'(sys_mkdir\(void\)\s*\{[^}]*?)(begin_op\(\);)',
        lambda m: m.group(1) + MKDIR_HOOK + m.group(2),
        sf, count=1, flags=re.DOTALL)
    if cnt == 1:
        if not os.path.exists(full_sf + ".orig"):
            shutil.copy(full_sf, full_sf + ".orig")
        open(full_sf, "w").write(new_sf)
        print("  ok    kernel/sysfile.c    fuse_do_mkdir")
    else:
        # Try alternate pattern: sys_mkdir with argstr + begin_op
        new_sf, cnt = re.subn(
            r'(sys_mkdir\b.*?argstr[^\n]+\n\s*(?:return\s*-1;\s*\n)?)(begin_op\(\);)',
            lambda m: m.group(1) + MKDIR_HOOK + m.group(2),
            sf, count=1, flags=re.DOTALL)
        if cnt == 1:
            if not os.path.exists(full_sf + ".orig"):
                shutil.copy(full_sf, full_sf + ".orig")
            open(full_sf, "w").write(new_sf)
            print("  ok    kernel/sysfile.c    fuse_do_mkdir (alt pattern)")
        else:
            print("  FAIL  kernel/sysfile.c    sys_mkdir begin_op anchor not found")
            bad += 1
else:
    print("  skip  kernel/sysfile.c    already has fuse_do_mkdir")

# Hook into sys_unlink
UNLINK_HOOK = """
  {
    char rel[FUSE_PATHMAX];
    int rr = fuse_route(path, rel);
    if(rr == -2)
      return -1;
    if(rr == 0)
      return fuse_do_unlink(rel);
  }
"""
sf = open(full_sf).read()
if "fuse_do_unlink" not in sf:
    new_sf, cnt = re.subn(
        r'(sys_unlink\(void\)\s*\{[^}]*?)(begin_op\(\);)',
        lambda m: m.group(1) + UNLINK_HOOK + m.group(2),
        sf, count=1, flags=re.DOTALL)
    if cnt == 1:
        open(full_sf, "w").write(new_sf)
        print("  ok    kernel/sysfile.c    fuse_do_unlink")
    else:
        new_sf, cnt = re.subn(
            r'(sys_unlink\b.*?argstr[^\n]+\n\s*(?:return\s*-1;\s*\n)?)(begin_op\(\);)',
            lambda m: m.group(1) + UNLINK_HOOK + m.group(2),
            sf, count=1, flags=re.DOTALL)
        if cnt == 1:
            open(full_sf, "w").write(new_sf)
            print("  ok    kernel/sysfile.c    fuse_do_unlink (alt pattern)")
        else:
            print("  FAIL  kernel/sysfile.c    sys_unlink begin_op anchor not found")
            bad += 1
else:
    print("  skip  kernel/sysfile.c    already has fuse_do_unlink")

# ---- kernel/proc.c : fail requests if the daemon exits
patch("kernel/proc.c", r"p->cwd = 0;\n",
      lambda m: m.group(0) + "  fuse_proc_exit(p->pid);\n", "fuse_proc_exit")

# ---- Makefile
patch("Makefile", r"(?m)^([ \t]*)\$K/file\.o[ \t]*\\\n",
      lambda m: m.group(0) + m.group(1) + "$K/fuse.o \\\n", "$K/fuse.o")
patch("Makefile", r"\$U/_zombie[ \t]*\\\n",
      lambda m: m.group(0) + "\t$U/_fused\\\n\t$U/_fusetest\\\n", "$U/_fused")

print("")
print("FAILED: %d patch(es) need manual editing (see README.md)" % bad if bad
      else "All patches applied successfully.")
sys.exit(1 if bad else 0)
PY
echo
echo "Next:  cd $ROOT && make clean && make qemu"
