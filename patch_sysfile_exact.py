import os

sysfile_path = os.path.expanduser("~/xv6-riscv/kernel/sysfile.c")
with open(sysfile_path, "r") as f:
    text = f.read()

idx1 = text.find("sys_mkdir(void)")
idx2 = text.find("sys_mknod(void)")
print("Found sys_mkdir from", idx1, "to", idx2)

new_mkdir = """sys_mkdir(void)
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
}

uint64
"""

updated = text[:idx1] + new_mkdir + text[idx2:]
with open(sysfile_path, "w") as f:
    f.write(updated)
print("sysfile.c successfully updated with exact slice replace!")
