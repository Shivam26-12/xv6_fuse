// fusetest.c - exercises /fuse through the normal file system calls.
//   fusetest        (daemon started with: fused hello &)
//   fusetest mem    (daemon started with: fused mem &)
//
// UPGRADED: tests hierarchical inode-based filesystem with mkdir support.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"

static int failures;

#define CHECK(c, msg) do { \
  if(c) printf("ok:   %s\n", msg); \
  else { printf("FAIL: %s\n", msg); failures++; } \
} while(0)

static char buf[4096];
static char expect[4096];

static void
fill(char *b, int len, int seed)
{
  int i;
  for(i = 0; i < len; i++)
    b[i] = (i * 7 + seed) % 251 + 1;
}

static int
writefile(char *path, int len, int seed)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0)
    return -1;
  fill(buf, len, seed);
  if(write(fd, buf, len) != len){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
checkfile(char *path, int len, int seed)
{
  int fd = open(path, O_RDONLY), n, tot = 0;
  char c;

  if(fd < 0)
    return -1;
  while(tot < len){
    n = read(fd, buf + tot, len - tot);
    if(n <= 0)
      break;
    tot += n;
  }
  fill(expect, len, seed);
  if(tot != len || memcmp(buf, expect, len) != 0){
    close(fd);
    return -1;
  }
  if(read(fd, &c, 1) != 0){
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
dir_has(char *dir, char *name)
{
  struct dirent de;
  char nm[DIRSIZ + 1];
  int fd = open(dir, O_RDONLY), found = 0;

  if(fd < 0)
    return 0;
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    memmove(nm, de.name, DIRSIZ);
    nm[DIRSIZ] = 0;
    if(strcmp(nm, name) == 0)
      found = 1;
  }
  close(fd);
  return found;
}

int
main(int argc, char *argv[])
{
  int fd, n, i, pid, status, bad;
  int mem = (argc > 1 && strcmp(argv[1], "mem") == 0);
  struct stat st;
  char name[32];

  printf("fusetest: %s mode\n", mem ? "mem (hierarchical inode)" : "hello");

  // ----------------------------------------------------------------
  // --- read the hello file ---
  fd = open("/fuse/hello", O_RDONLY);
  CHECK(fd >= 0, "open /fuse/hello");
  if(fd >= 0){
    n = read(fd, buf, sizeof(buf) - 1);
    if(n < 0) n = 0;
    buf[n] = 0;
    CHECK(strcmp(buf, "Hello from the xv6 FUSE daemon!\n") == 0,
          "read returns file contents");
    CHECK(fstat(fd, &st) == 0 && st.type == T_FILE && st.size == n,
          "fstat reports type and size");
    CHECK(read(fd, buf, 10) == 0, "read at end of file returns 0");
    close(fd);
  }

  // ----------------------------------------------------------------
  // --- errors and routing ---
  fd = open("/fuse/nope", O_RDONLY);
  CHECK(fd < 0, "open of a missing file fails");
  if(fd >= 0) close(fd);
  fd = open("/README", O_RDONLY);
  CHECK(fd >= 0, "paths outside /fuse still use the disk");
  if(fd >= 0) close(fd);

  // ----------------------------------------------------------------
  // --- directory ---
  fd = open("/fuse", O_RDONLY);
  CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.type == T_DIR,
        "/fuse is a directory");
  if(fd >= 0) close(fd);
  CHECK(dir_has("/fuse", "hello"), "readdir lists hello");

  // ----------------------------------------------------------------
  if(!mem){
    fd = open("/fuse/hello", O_WRONLY);
    CHECK(fd < 0, "write to the read-only file system is refused");
    if(fd >= 0) close(fd);
    printf("fusetest: hello mode tests done\n");
  } else {
    // ----------------------------------------------------------------
    // --- write path: 3000 bytes = multiple request chunks ---
    CHECK(writefile("/fuse/t1", 3000, 1) == 0, "create + write 3000 bytes");
    CHECK(checkfile("/fuse/t1", 3000, 1) == 0, "read back 3000 bytes, contents identical");
    CHECK(dir_has("/fuse", "t1"), "readdir lists the new file");
    fd = open("/fuse/t1", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.size == 3000, "fstat size is 3000");
    if(fd >= 0) close(fd);
    CHECK(writefile("/fuse/t1", 100, 2) == 0 && checkfile("/fuse/t1", 100, 2) == 0,
          "O_TRUNC then rewrite gives 100 bytes");

    // ----------------------------------------------------------------
    // --- HIERARCHICAL: mkdir creates a subdirectory ---
    CHECK(mkdir("/fuse/subdir") == 0, "mkdir /fuse/subdir succeeds");
    CHECK(dir_has("/fuse", "subdir"), "readdir lists subdir");
    fd = open("/fuse/subdir", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.type == T_DIR,
          "/fuse/subdir is a directory");
    if(fd >= 0) close(fd);

    // ----------------------------------------------------------------
    // --- HIERARCHICAL: create file inside subdirectory ---
    CHECK(writefile("/fuse/subdir/file1", 500, 42) == 0,
          "create + write /fuse/subdir/file1");
    CHECK(checkfile("/fuse/subdir/file1", 500, 42) == 0,
          "read back /fuse/subdir/file1 contents");
    CHECK(dir_has("/fuse/subdir", "file1"),
          "readdir of /fuse/subdir lists file1");
    fd = open("/fuse/subdir/file1", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.size == 500,
          "fstat /fuse/subdir/file1 size is 500");
    if(fd >= 0) close(fd);

    // ----------------------------------------------------------------
    // --- HIERARCHICAL: nested subdirectory ---
    CHECK(mkdir("/fuse/subdir/nested") == 0,
          "mkdir /fuse/subdir/nested succeeds");
    CHECK(dir_has("/fuse/subdir", "nested"),
          "readdir of /fuse/subdir lists nested");
    fd = open("/fuse/subdir/nested", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.type == T_DIR,
          "/fuse/subdir/nested is a directory");
    if(fd >= 0) close(fd);

    // ----------------------------------------------------------------
    // --- HIERARCHICAL: file in nested directory ---
    CHECK(writefile("/fuse/subdir/nested/deep", 200, 7) == 0,
          "create + write /fuse/subdir/nested/deep");
    CHECK(checkfile("/fuse/subdir/nested/deep", 200, 7) == 0,
          "read back /fuse/subdir/nested/deep");
    CHECK(dir_has("/fuse/subdir/nested", "deep"),
          "readdir of /fuse/subdir/nested lists deep");

    // ----------------------------------------------------------------
    // --- HIERARCHICAL: multiple directories at root level ---
    CHECK(mkdir("/fuse/docs") == 0, "mkdir /fuse/docs");
    CHECK(mkdir("/fuse/src")  == 0, "mkdir /fuse/src");
    CHECK(dir_has("/fuse", "docs") && dir_has("/fuse", "src"),
          "readdir lists docs and src");
    CHECK(writefile("/fuse/docs/readme", 128, 11) == 0,
          "create /fuse/docs/readme");
    CHECK(writefile("/fuse/src/main", 256, 22) == 0,
          "create /fuse/src/main");
    CHECK(checkfile("/fuse/docs/readme", 128, 11) == 0 &&
          checkfile("/fuse/src/main",    256, 22) == 0,
          "read back docs/readme and src/main");

    // ----------------------------------------------------------------
    // --- concurrency: 4 processes hammer the daemon at once ---
    for(i = 0; i < 4; i++){
      pid = fork();
      if(pid < 0){
        printf("fork failed\n");
        exit(1);
      }
      if(pid == 0){
        int k, badc = 0;
        // each child works in its own subdirectory
        char dname[32], fname[48];
        strcpy(dname, "/fuse/proc0");
        dname[10] = '0' + i;
        if(mkdir(dname) < 0){ exit(1); }
        strcpy(fname, dname);
        fname[11] = '/';
        fname[12] = 'f';
        fname[13] = 0;
        for(k = 0; k < 3; k++)
          if(writefile(fname, 1000 + k * 100, i + k) < 0 ||
             checkfile(fname, 1000 + k * 100, i + k) < 0)
            badc = 1;
        exit(badc);
      }
    }
    bad = 0;
    for(i = 0; i < 4; i++){
      wait(&status);
      if(status != 0)
        bad++;
    }
    CHECK(bad == 0,
          "4 concurrent processes each create a subdir and write/verify files");

    // verify all dirs exist
    name[0] = 0;
    strcpy(name, "/fuse/proc0");
    name[10] = '0';
    CHECK(dir_has("/fuse", "proc0") && dir_has("/fuse", "proc3"),
          "concurrent subdirectories proc0..proc3 all present");
  }

  if(failures == 0)
    printf("fusetest: ALL PASSED\n");
  else
    printf("fusetest: %d FAILED\n", failures);
  exit(failures != 0);
}
