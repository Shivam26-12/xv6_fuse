// fused.c - FUSE daemon for xv6 with HIERARCHICAL inode-based filesystem.
//
// Modes:
//   fused hello [-v] &     read-only "hello" filesystem (one file: /fuse/hello)
//   fused mem   [-v] &     read-write in-memory HIERARCHICAL filesystem
//                          supports: mkdir, file create/read/write, ls, nested dirs
//
// Design:
//   Each inode has a numeric id (ino), a type (T_FILE or T_DIR), a name,
//   a parent inode id, and either file data or a list of child ino ids.
//   The root directory is inode 1 (ino=1, name="/", parent=1).
//
//   Path resolution walks the inode tree from root by splitting on '/'.
//
// Protocol:
//   op FUSE_OPEN    -> inode lookup (or create if O_CREATE); reply: type, fh=ino
//   op FUSE_GETATTR -> stat by path; reply: type, size
//   op FUSE_READ    -> file read; reply: data bytes
//   op FUSE_WRITE   -> file write; reply: bytes accepted
//   op FUSE_READDIR -> directory listing as packed struct dirent[]; reply: data
//   op FUSE_MKDIR   -> create directory at path; reply: err
//   op FUSE_UNLINK  -> remove file at path; reply: err
//   op FUSE_RMDIR   -> remove empty dir at path; reply: err
//   op FUSE_RELEASE -> close handle; no reply

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/fuse.h"

// ---- inode table -------------------------------------------------------

#define MAXINODES  128      // max total inodes (files + directories)
#define MAXFSIZE   4096     // max file data bytes
#define MAXCHILDREN 32      // max children per directory

struct inode {
  int    used;
  uint   ino;              // inode number (1-based, 0 = free)
  short  type;             // T_FILE or T_DIR
  char   name[DIRSIZ + 1]; // entry name (not full path)
  uint   parent;           // parent ino (root's parent = itself)
  // file data
  int    size;
  char   data[MAXFSIZE];
  // directory children: array of child ino numbers
  int    nchildren;
  uint   children[MAXCHILDREN];
};

static struct inode inodes[MAXINODES];
static uint  nextino = 1;   // next free inode number to assign

// Find inode by ino number; returns pointer or 0.
static struct inode *
inode_get(uint ino)
{
  int i;
  for(i = 0; i < MAXINODES; i++)
    if(inodes[i].used && inodes[i].ino == ino)
      return &inodes[i];
  return 0;
}

// Allocate a new inode; returns pointer or 0 if full.
static struct inode *
inode_alloc(void)
{
  int i;
  for(i = 0; i < MAXINODES; i++){
    if(!inodes[i].used){
      memset(&inodes[i], 0, sizeof(inodes[i]));
      inodes[i].used = 1;
      inodes[i].ino  = ++nextino;
      return &inodes[i];
    }
  }
  return 0;
}

// Free an inode.
static void
inode_free(struct inode *ip)
{
  ip->used = 0;
}

// Add child ino to parent directory. Returns 0 on success, -1 if full.
static int
dir_add_child(struct inode *parent, uint child_ino)
{
  if(parent->nchildren >= MAXCHILDREN)
    return -1;
  parent->children[parent->nchildren++] = child_ino;
  return 0;
}

// Remove child ino from parent directory. Returns 0 on success, -1 if not found.
static int
dir_remove_child(struct inode *parent, uint child_ino)
{
  int i;
  for(i = 0; i < parent->nchildren; i++){
    if(parent->children[i] == child_ino){
      parent->children[i] = parent->children[--parent->nchildren];
      return 0;
    }
  }
  return -1;
}

// Look up a child by name in a directory. Returns child inode or 0.
static struct inode *
dir_lookup(struct inode *dir, char *name)
{
  int i;
  for(i = 0; i < dir->nchildren; i++){
    struct inode *child = inode_get(dir->children[i]);
    if(child && strcmp(child->name, name) == 0)
      return child;
  }
  return 0;
}

// ---- path resolution ---------------------------------------------------

// Walk a path like "/" or "/a/b/c" starting from root inode.
// Returns the inode for the final component, or 0 if not found.
//
// parent_out: on return, points to the PARENT of the last component.
//   - If the last component was found:    parent of that inode.
//   - If the last component is MISSING:   the inode it should be in
//                                         (useful for create/mkdir).
//   - If an INTERMEDIATE component is missing: set to 0 (hard failure).
//
// lastname: the name of the last component (not the full path).
static struct inode *
path_lookup(char *path, struct inode **parent_out, char *lastname)
{
  struct inode *root, *cur, *next;
  char   comp[DIRSIZ + 1];
  char  *p = path;
  int    n;

  // Clear outputs
  if(parent_out) *parent_out = 0;
  if(lastname)   lastname[0] = 0;

  root = inode_get(1);   // root is always ino=1
  if(!root) return 0;

  if(*p == '/') p++;

  if(*p == 0){
    // path is "/" (root itself)
    if(parent_out) *parent_out = root;
    return root;
  }

  cur = root;

  while(*p){
    // Extract next path component
    n = 0;
    while(*p && *p != '/' && n < DIRSIZ)
      comp[n++] = *p++;
    comp[n] = 0;
    if(*p == '/') p++;

    // Skip . (stay) and .. (go up)
    if(strcmp(comp, ".") == 0)
      continue;
    if(strcmp(comp, "..") == 0){
      cur = inode_get(cur->parent);
      if(!cur) return 0;
      continue;
    }

    if(cur->type != T_DIR)
      return 0;

    // Remember this parent before looking up the child
    struct inode *this_parent = cur;

    next = dir_lookup(cur, comp);
    if(!next){
      // Component not found.
      if(*p == 0){
        // This is the LAST component — OK for creation.
        // Return the parent so the caller can create here.
        if(parent_out) *parent_out = this_parent;
        if(lastname){
          int k;
          for(k = 0; k <= n && k < DIRSIZ; k++)
            lastname[k] = comp[k];
          lastname[n] = 0;
        }
      }
      // If intermediate component missing, parent_out stays 0 → hard fail.
      return 0;
    }
    cur = next;
  }

  // Found the inode — also set parent and lastname for the caller.
  if(parent_out) *parent_out = inode_get(cur->parent);
  if(lastname){
    int k;
    for(k = 0; k < DIRSIZ && cur->name[k]; k++)
      lastname[k] = cur->name[k];
    lastname[k] = 0;
  }
  return cur;
}

// ---- misc helpers ------------------------------------------------------

static int verbose;
static int readonly;

static struct fuse_req req;
static struct fuse_rep rep;

static const char *opname[] = {
  "?", "OPEN", "READ", "WRITE", "READDIR", "GETATTR", "RELEASE",
  "MKDIR", "UNLINK", "RMDIR"
};

static void
mcopy(char *dst, const char *src, int max)
{
  int i;
  for(i = 0; i < max - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static void
setname(char *dst, const char *src)
{
  int i = 0;
  while(i < DIRSIZ && src[i]){ dst[i] = src[i]; i++; }
  while(i < DIRSIZ) dst[i++] = 0;
}

static int
isroot(char *p)
{
  return strcmp(p, "/") == 0 || strcmp(p, "/.") == 0 || strcmp(p, "/..") == 0;
}

// ---- init --------------------------------------------------------------

static void
init_root(void)
{
  struct inode *root;
  memset(inodes, 0, sizeof(inodes));
  // allocate root with ino=1 manually
  root = &inodes[0];
  root->used   = 1;
  root->ino    = 1;
  root->type   = T_DIR;
  root->parent = 1;      // root's parent is itself
  mcopy(root->name, "/", DIRSIZ + 1);
  root->nchildren = 0;
  nextino = 1;           // next alloc will give ino=2
}

static void
init_hello_file(void)
{
  // Create /hello file for the hello mode
  struct inode *root = inode_get(1);
  struct inode *f = inode_alloc();
  if(!root || !f) return;
  static char msg[] = "Hello from the xv6 FUSE daemon!\n";
  f->type   = T_FILE;
  f->parent = root->ino;
  mcopy(f->name, "hello", DIRSIZ + 1);
  mcopy(f->data, msg, MAXFSIZE);
  f->size   = strlen(msg);
  dir_add_child(root, f->ino);
}

// ---- Build dirent listing for a directory inode -----------------------
// Returns count of entries written into ents[].
static struct dirent ents[MAXCHILDREN + 2];

static int
build_dir_listing(struct inode *dir)
{
  int n = 0, i;
  struct inode *child;

  memset(ents, 0, sizeof(ents));
  // . entry
  ents[n].inum = dir->ino;
  setname(ents[n++].name, ".");
  // .. entry
  ents[n].inum = dir->parent;
  setname(ents[n++].name, "..");
  // children
  for(i = 0; i < dir->nchildren; i++){
    child = inode_get(dir->children[i]);
    if(child){
      ents[n].inum = child->ino;
      setname(ents[n++].name, child->name);
    }
  }
  return n;
}

// ---- handle() ----------------------------------------------------------

static void
handle(void)
{
  char *path = req.path;
  int   flags = req.flags;
  int   off  = req.off;
  int   len  = req.len;
  struct inode *ip, *parent, *newip;
  char   lastname[DIRSIZ + 1];
  int    n, total;

  rep.id = req.id;

  switch(req.op){

  // ----------------------------------------------------------------
  case FUSE_OPEN:
    if(isroot(path)){
      rep.type = T_DIR;
      rep.fh   = 1;
      break;
    }
    ip = path_lookup(path, &parent, lastname);
    if(ip){
      // file/dir found
      if(readonly && (flags & (O_WRONLY | O_RDWR | O_TRUNC))){
        rep.err = -1; break;
      }
      if(flags & O_TRUNC && ip->type == T_FILE)
        ip->size = 0;
      rep.type = ip->type;
      rep.fh   = ip->ino;
    } else {
      // not found: create if O_CREATE and not readonly and it's a file
      if(!(flags & O_CREATE) || readonly){
        rep.err = -1; break;
      }
      if(!parent || parent->type != T_DIR){
        rep.err = -1; break;
      }
      if(lastname[0] == 0 || strlen(lastname) > DIRSIZ){
        rep.err = -1; break;
      }
      newip = inode_alloc();
      if(!newip){ rep.err = -1; break; }
      newip->type   = T_FILE;
      newip->parent = parent->ino;
      mcopy(newip->name, lastname, DIRSIZ + 1);
      newip->size   = 0;
      if(dir_add_child(parent, newip->ino) < 0){
        inode_free(newip); rep.err = -1; break;
      }
      rep.type = T_FILE;
      rep.fh   = newip->ino;
    }
    break;

  // ----------------------------------------------------------------
  case FUSE_GETATTR:
    if(isroot(path)){
      struct inode *root = inode_get(1);
      rep.type = T_DIR;
      rep.size = root ? (build_dir_listing(root) * sizeof(struct dirent)) : 0;
      rep.fh   = 1;
      break;
    }
    ip = path_lookup(path, 0, 0);
    if(!ip){ rep.err = -1; break; }
    rep.type = ip->type;
    rep.fh   = ip->ino;
    if(ip->type == T_DIR)
      rep.size = build_dir_listing(ip) * sizeof(struct dirent);
    else
      rep.size = ip->size;
    break;

  // ----------------------------------------------------------------
  case FUSE_READ:
    ip = path_lookup(path, 0, 0);
    if(!ip || ip->type != T_FILE){ rep.err = -1; break; }
    if(off >= ip->size){ rep.len = 0; break; }
    n = ip->size - off;
    if(n > len) n = len;
    if(n > FUSE_DATAMAX) n = FUSE_DATAMAX;
    memmove(rep.data, ip->data + off, n);
    rep.len = n;
    break;

  // ----------------------------------------------------------------
  case FUSE_WRITE:
    ip = path_lookup(path, 0, 0);
    if(readonly || !ip || ip->type != T_FILE || off >= MAXFSIZE){
      rep.err = -1; break;
    }
    n = len;
    if(n > FUSE_DATAMAX) n = FUSE_DATAMAX;
    if(off + n > MAXFSIZE) n = MAXFSIZE - off;
    if(off > ip->size)
      memset(ip->data + ip->size, 0, off - ip->size);
    memmove(ip->data + off, req.data, n);
    if(off + n > ip->size) ip->size = off + n;
    rep.len = n;
    break;

  // ----------------------------------------------------------------
  case FUSE_READDIR:
    ip = path_lookup(path, 0, 0);
    if(!ip || ip->type != T_DIR){ rep.err = -1; break; }
    total = build_dir_listing(ip) * sizeof(struct dirent);
    if(off >= total){ rep.len = 0; break; }
    n = total - off;
    if(n > len) n = len;
    if(n > FUSE_DATAMAX) n = FUSE_DATAMAX;
    memmove(rep.data, (char*)ents + off, n);
    rep.len = n;
    break;

  // ----------------------------------------------------------------
  case FUSE_MKDIR:
    if(readonly){ rep.err = -1; break; }
    // path must not exist yet
    ip = path_lookup(path, &parent, lastname);
    if(ip){ rep.err = -1; break; }   // already exists
    if(!parent || parent->type != T_DIR){ rep.err = -1; break; }
    if(lastname[0] == 0 || strlen(lastname) > DIRSIZ){ rep.err = -1; break; }
    newip = inode_alloc();
    if(!newip){ rep.err = -1; break; }
    newip->type   = T_DIR;
    newip->parent = parent->ino;
    mcopy(newip->name, lastname, DIRSIZ + 1);
    newip->nchildren = 0;
    if(dir_add_child(parent, newip->ino) < 0){
      inode_free(newip); rep.err = -1; break;
    }
    break;

  // ----------------------------------------------------------------
  case FUSE_UNLINK:
    if(readonly){ rep.err = -1; break; }
    ip = path_lookup(path, &parent, lastname);
    if(!ip || ip->type != T_FILE){ rep.err = -1; break; }
    if(!parent){ rep.err = -1; break; }
    dir_remove_child(parent, ip->ino);
    inode_free(ip);
    break;

  // ----------------------------------------------------------------
  case FUSE_RMDIR:
    if(readonly){ rep.err = -1; break; }
    ip = path_lookup(path, &parent, lastname);
    if(!ip || ip->type != T_DIR){ rep.err = -1; break; }
    if(ip->ino == 1){ rep.err = -1; break; }   // can't remove root
    if(ip->nchildren > 0){ rep.err = -1; break; }  // must be empty
    if(!parent){ rep.err = -1; break; }
    dir_remove_child(parent, ip->ino);
    inode_free(ip);
    break;

  // ----------------------------------------------------------------
  case FUSE_RELEASE:
    break;  // nothing to clean up

  default:
    rep.err = -1;
  }
}

// ---- main --------------------------------------------------------------

int
main(int argc, char *argv[])
{
  int fd, n, i;

  readonly = 1;
  verbose  = 0;
  for(i = 1; i < argc; i++){
    if(strcmp(argv[i], "mem") == 0)
      readonly = 0;
    else if(strcmp(argv[i], "-v") == 0)
      verbose = 1;
  }

  init_root();
  init_hello_file();
  if(!readonly)
    printf("fused: in-memory hierarchical fs (read-write, inode-based)\n");
  else {
    printf("fused: hello read-only fs\n");
  }

  fd = open("/dev/fuse", O_RDWR);
  if(fd < 0){
    mkdir("/dev");
    mknod("/dev/fuse", FUSEDEV, 0);
    fd = open("/dev/fuse", O_RDWR);
  }
  if(fd < 0){
    printf("fused: cannot open /dev/fuse\n");
    exit(1);
  }
  printf("fused: serving /fuse (%s)\n", readonly ? "hello, read-only" : "in-memory hierarchical, read-write");

  for(;;){
    n = read(fd, &req, sizeof(req));
    if(n != sizeof(req)){
      printf("fused: read from /dev/fuse failed, exiting\n");
      exit(1);
    }
    memset(&rep, 0, sizeof(rep));
    handle();
    if(verbose){
      int op = req.op;
      printf("fused: #%d %s %s off=%d len=%d -> %s\n",
             req.id,
             (op >= 1 && op <= 9) ? opname[op] : "?",
             req.path, req.off, req.len,
             rep.err ? "ERR" : "ok");
    }
    if(req.op == FUSE_RELEASE)
      continue;    // one-way message, no reply
    n = FUSE_REP_HDR + (rep.len > 0 ? rep.len : 0);
    if(write(fd, &rep, n) != n)
      printf("fused: reply write failed\n");
  }
}
