// fuse.h - request/reply protocol shared by the kernel (fuse.c) and the
// user-space daemon (user/fused.c).
//
// UPGRADED: Hierarchical inode-based filesystem with mkdir support.
#ifndef XV6_FUSE_H
#define XV6_FUSE_H

#define FUSEDEV       2     // major number of /dev/fuse
#define FUSE_PATHMAX  128   // max path length *relative to the mount point* (enlarged for deep paths)
#define FUSE_DATAMAX  512   // max payload per request/reply

// operations
#define FUSE_OPEN     1
#define FUSE_READ     2
#define FUSE_WRITE    3
#define FUSE_READDIR  4
#define FUSE_GETATTR  5
#define FUSE_RELEASE  6     // close; one-way, daemon must NOT reply
#define FUSE_MKDIR    7     // create a directory
#define FUSE_UNLINK   8     // remove a file
#define FUSE_RMDIR    9     // remove a directory (must be empty)

// kernel -> daemon: the daemon read()s one of these from /dev/fuse
struct fuse_req {
  uint id;                  // unique request id, echo it in the reply
  uint op;                  // FUSE_*
  uint fh;                  // handle the daemon returned from OPEN
  uint off;                 // file offset (READ/WRITE/READDIR)
  uint len;                 // bytes wanted (READ/READDIR) or in data[] (WRITE)
  uint flags;               // open flags (OPEN)
  char path[FUSE_PATHMAX];  // "/" or "/name" or "/dir/name", relative to the mount point
  char data[FUSE_DATAMAX];  // WRITE payload
};

// daemon -> kernel: the daemon write()s one of these to /dev/fuse.
// It may write only FUSE_REP_HDR + len bytes.
#define FUSE_REP_HDR 24
struct fuse_rep {
  uint id;                  // copied from the request
  int  err;                 // 0 = ok, nonzero = failure
  int  len;                 // bytes valid in data[] (READ/READDIR/WRITE count)
  uint type;                // OPEN/GETATTR: T_FILE or T_DIR
  uint size;                // GETATTR: size in bytes
  uint fh;                  // OPEN: handle for later requests
  char data[FUSE_DATAMAX];
};

#endif
