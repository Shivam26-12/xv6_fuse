import re

sysfile_path = "/home/shivam/xv6-riscv/kernel/sysfile.c"
with open(sysfile_path, "r") as f:
    sf = f.read()

new_mkdir = """uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  {
    // route mkdir under a FUSE mount point to the daemon
    char rel[FUSE_PATHMAX];
    int rr = fuse_route(path, rel);
    if(rr == -2)
      return -1;
    if(rr == 0)
      return fuse_do_mkdir(rel);
  }

  begin_op();
  if ((ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}"""

sf = re.sub(r'uint64\s*\nsys_mkdir\(void\)\s*\{[^}]*?iunlockput\(ip\);[^}]*?\}', new_mkdir, sf, flags=re.DOTALL)

with open(sysfile_path, "w") as f:
    f.write(sf)
print("sysfile.c updated successfully")

# Now patch file.c
file_c_path = "/home/shivam/xv6-riscv/kernel/file.c"
with open(file_c_path, "r") as f:
    fc = f.read()

# 1. Add fuseinit() in fileinit if not present
if "fuseinit();" not in fc:
    fc = fc.replace('initlock(&ftable.lock, "ftable");', 'initlock(&ftable.lock, "ftable");\n  fuseinit();')

# 2. Add fuse_release in fileclose
if "fuse_release" not in fc:
    fc = fc.replace(
        "} else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {",
        "} else if (ff.type == FD_FUSE) {\n    fuse_release(&ff);\n  } else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {"
    )

# 3. Add fuse_getattr in filestat
if "fuse_getattr" not in fc:
    filestat_target = """int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if (f->type == FD_INODE || f->type == FD_DEVICE) {"""

    filestat_replacement = """int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if (f->type == FD_FUSE) {
    struct stat fst;
    if (fuse_getattr(f, &fst) < 0)
      return -1;
    if (either_copyout(1, addr, (char *)&fst, sizeof(fst)) < 0)
      return -1;
    return 0;
  }

  if (f->type == FD_INODE || f->type == FD_DEVICE) {"""
    fc = fc.replace(filestat_target, filestat_replacement)

with open(file_c_path, "w") as f:
    f.write(fc)
print("file.c updated successfully")
