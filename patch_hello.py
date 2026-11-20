import os

fused_path = os.path.expanduser("~/xv6-riscv/user/fused.c")
with open(fused_path, "r") as f:
    text = f.read()

# In main(): change:
# if(!readonly)
#   printf(...)
# else {
#   init_hello_file();
#   ...
# }
# to:
# init_hello_file(); in both modes!

old_init = """  init_root();
  if(!readonly)
    printf("fused: in-memory hierarchical fs (read-write, inode-based)\\n");
  else {
    init_hello_file();
    printf("fused: hello read-only fs\\n");
  }"""

new_init = """  init_root();
  init_hello_file();
  if(!readonly)
    printf("fused: in-memory hierarchical fs (read-write, inode-based)\\n");
  else {
    printf("fused: hello read-only fs\\n");
  }"""

if old_init in text:
    text = text.replace(old_init, new_init, 1)
    with open(fused_path, "w") as f:
        f.write(text)
    print("fused.c updated to create hello in both modes!")
else:
    print("Pattern not found in fused.c")

# Also copy to windows workspace
win_fused = "/mnt/c/Users/SHIVAM BHATT/Downloads/Project10_Part2_xv6_FUSE_Complete/Project10_Part2_xv6_FUSE/user/fused.c"
with open(win_fused, "w") as f:
    f.write(text)
print("Updated windows copy as well")
