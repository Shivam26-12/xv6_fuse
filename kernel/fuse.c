//
// fuse.c - a minimal "FUSE for xv6".
//
// UPGRADED: Hierarchical inode-based filesystem with mkdir support.
//   - Added FUSE_MKDIR, FUSE_UNLINK, FUSE_RMDIR operations
//   - FUSE_PATHMAX enlarged to 128 for deep directory paths
//   - fuse_do_mkdir() dispatches mkdir for paths under a mount point
//   - fuse_do_unlink() dispatches unlink for paths under a mount point
//
// PORTABILITY NOTE:
//   This file uses yield()-based polling for waiting instead of
//   sleep(chan, lock).  This works across all xv6 variants regardless
//   of the sleep() signature used in the kernel.  It is slightly less
//   efficient than sleep/wakeup but is fully correct.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "fuse.h"

_Static_assert(sizeof(((struct file *)0)->fuse_path) == FUSE_PATHMAX,
               "file.h fuse_path size must equal FUSE_PATHMAX");

#define FUSE_NSLOT 8        // max requests in flight at once

enum { SL_FREE, SL_FILL, SL_PENDING, SL_INFLIGHT, SL_DONE, SL_ABANDON };

// One slot = one request plus its reply.
struct fuse_slot {
  int state;
  struct fuse_req req;
  struct fuse_rep rep;
};

static struct {
  struct spinlock lock;
  struct fuse_slot slot[FUSE_NSLOT];
  uint nextid;
  int daemon_pid;           // 0 = no daemon attached
} fuse;

// ---- mount table ---------------------------------------------------
struct mount {
  char path[16];
  int  dev;
};
static struct mount mtab[] = {
  { "/fuse", FUSEDEV },
};
#define NMOUNT ((int)(sizeof(mtab) / sizeof(mtab[0])))

// If path lies under a mount point, copy the remainder into rel and
// return 0.  -1: not mounted.  -2: mounted but unusable.
int
fuse_route(char *path, char *rel)
{
  int i, l;
  char *p;

  for(i = 0; i < NMOUNT; i++){
    l = strlen(mtab[i].path);
    if(strncmp(path, mtab[i].path, l) != 0)
      continue;
    if(path[l] != '/' && path[l] != 0)
      continue;
    if(devsw[mtab[i].dev].read == 0)
      return -2;
    p = path + l;
    if(*p == 0)
      p = "/";
    if(strlen(p) >= FUSE_PATHMAX)
      return -2;
    safestrcpy(rel, p, FUSE_PATHMAX);
    return 0;
  }
  return -1;
}

// ---- slot management (yield-based, no sleep/wakeup needed) ---------

// Reserve a free slot.  Spins with yield() until one is available.
// Returns 0 if no daemon is attached or the process is killed.
static struct fuse_slot *
fuse_begin(void)
{
  int i;

  for(;;){
    acquire(&fuse.lock);
    if(fuse.daemon_pid == 0){
      release(&fuse.lock);
      return 0;
    }
    for(i = 0; i < FUSE_NSLOT; i++){
      if(fuse.slot[i].state == SL_FREE){
        fuse.slot[i].state = SL_FILL;
        release(&fuse.lock);
        memset(&fuse.slot[i].req, 0, sizeof(fuse.slot[i].req));
        return &fuse.slot[i];
      }
    }
    if(killed(myproc())){
      release(&fuse.lock);
      return 0;
    }
    release(&fuse.lock);
    yield();              // give the daemon a chance to run
  }
}

static void
fuse_end(struct fuse_slot *s)
{
  acquire(&fuse.lock);
  s->state = SL_FREE;
  release(&fuse.lock);
}

// Mark request PENDING and spin with yield() until the daemon replies
// (state == SL_DONE).  Returns 0 on success, -1 if killed or no daemon.
static int
fuse_submit(struct fuse_slot *s)
{
  acquire(&fuse.lock);
  if(fuse.daemon_pid == 0){
    s->state = SL_FREE;
    release(&fuse.lock);
    return -1;
  }
  s->req.id = ++fuse.nextid;
  s->state = SL_PENDING;
  release(&fuse.lock);

  for(;;){
    acquire(&fuse.lock);
    if(s->state == SL_DONE){
      release(&fuse.lock);
      return 0;
    }
    if(fuse.daemon_pid == 0){
      // daemon died while we waited
      if(s->state == SL_PENDING || s->state == SL_INFLIGHT){
        s->rep.err = -1;
        s->rep.len = 0;
        s->state = SL_FREE;
      }
      release(&fuse.lock);
      return -1;
    }
    if(killed(myproc())){
      if(s->state == SL_PENDING)
        s->state = SL_FREE;
      else if(s->state == SL_INFLIGHT)
        s->state = SL_ABANDON;
      release(&fuse.lock);
      return -1;
    }
    release(&fuse.lock);
    yield();              // let daemon run so it can reply
  }
}

static void
fillreq(struct fuse_slot *s, uint op, struct file *f, uint off, uint len)
{
  s->req.op  = op;
  s->req.fh  = f->fuse_fh;
  s->req.off = off;
  s->req.len = len;
  safestrcpy(s->req.path, f->fuse_path, FUSE_PATHMAX);
}

// ---- kernel-side file operations ------------------------------------

int
fuse_do_open(char *rel, int flags, int *type, int *fh)
{
  struct fuse_slot *s;
  int r;

  if((s = fuse_begin()) == 0)
    return -1;
  s->req.op    = FUSE_OPEN;
  s->req.flags = flags;
  safestrcpy(s->req.path, rel, FUSE_PATHMAX);
  if(fuse_submit(s) < 0)
    return -1;
  r = s->rep.err;
  if(r == 0){
    *type = s->rep.type;
    *fh   = s->rep.fh;
  }
  fuse_end(s);
  return r ? -1 : 0;
}

int
fuse_do_mkdir(char *rel)
{
  struct fuse_slot *s;
  int r;

  if((s = fuse_begin()) == 0)
    return -1;
  s->req.op = FUSE_MKDIR;
  safestrcpy(s->req.path, rel, FUSE_PATHMAX);
  if(fuse_submit(s) < 0)
    return -1;
  r = s->rep.err;
  fuse_end(s);
  return r ? -1 : 0;
}

int
fuse_do_unlink(char *rel)
{
  struct fuse_slot *s;
  int r;

  if((s = fuse_begin()) == 0)
    return -1;
  s->req.op = FUSE_UNLINK;
  safestrcpy(s->req.path, rel, FUSE_PATHMAX);
  if(fuse_submit(s) < 0)
    return -1;
  r = s->rep.err;
  fuse_end(s);
  return r ? -1 : 0;
}

int
fuse_read(struct file *f, uint64 addr, int n)
{
  struct fuse_slot *s;
  int tot = 0, m, r;

  while(tot < n){
    m = n - tot;
    if(m > FUSE_DATAMAX)
      m = FUSE_DATAMAX;
    if((s = fuse_begin()) == 0)
      return tot > 0 ? tot : -1;
    fillreq(s, f->fuse_type == T_DIR ? FUSE_READDIR : FUSE_READ, f, f->off, m);
    if(fuse_submit(s) < 0)
      return tot > 0 ? tot : -1;
    r = s->rep.err ? -1 : s->rep.len;
    if(r > m) r = -1;
    if(r > 0 && either_copyout(1, addr + tot, s->rep.data, r) < 0)
      r = -1;
    fuse_end(s);
    if(r < 0)
      return tot > 0 ? tot : -1;
    f->off += r;
    tot    += r;
    if(r < m)
      break;              // short read = EOF
  }
  return tot;
}

int
fuse_write(struct file *f, uint64 addr, int n)
{
  struct fuse_slot *s;
  int tot = 0, m, r;

  if(f->fuse_type == T_DIR)
    return -1;
  while(tot < n){
    m = n - tot;
    if(m > FUSE_DATAMAX)
      m = FUSE_DATAMAX;
    if((s = fuse_begin()) == 0)
      return tot > 0 ? tot : -1;
    fillreq(s, FUSE_WRITE, f, f->off, m);
    if(either_copyin(s->req.data, 1, addr + tot, m) < 0){
      fuse_end(s);
      return tot > 0 ? tot : -1;
    }
    if(fuse_submit(s) < 0)
      return tot > 0 ? tot : -1;
    r = s->rep.err ? -1 : s->rep.len;
    if(r > m) r = -1;
    fuse_end(s);
    if(r < 0)
      return tot > 0 ? tot : -1;
    f->off += r;
    tot    += r;
    if(r < m)
      break;              // partial write = "disk full"
  }
  return tot;
}

int
fuse_getattr(struct file *f, struct stat *st)
{
  struct fuse_slot *s;
  int r;

  if((s = fuse_begin()) == 0)
    return -1;
  fillreq(s, FUSE_GETATTR, f, 0, 0);
  if(fuse_submit(s) < 0)
    return -1;
  r = s->rep.err;
  if(r == 0){
    st->dev   = FUSEDEV;
    st->ino   = f->fuse_fh;
    st->type  = s->rep.type;
    st->nlink = 1;
    st->size  = s->rep.size;
  }
  fuse_end(s);
  return r ? -1 : 0;
}

// Close: one-way fire-and-forget.  Try to grab a free slot; if none
// available just skip — the daemon will never know but that's fine.
void
fuse_release(struct file *f)
{
  struct fuse_slot *s = 0;
  int i;

  acquire(&fuse.lock);
  if(fuse.daemon_pid != 0){
    for(i = 0; i < FUSE_NSLOT; i++){
      if(fuse.slot[i].state == SL_FREE){
        s = &fuse.slot[i];
        break;
      }
    }
  }
  if(s){
    memset(&s->req, 0, sizeof(s->req));
    s->req.id = ++fuse.nextid;
    s->req.op = FUSE_RELEASE;
    s->req.fh = f->fuse_fh;
    safestrcpy(s->req.path, f->fuse_path, FUSE_PATHMAX);
    s->state  = SL_PENDING;
    // No wakeup needed — daemon polls with yield() too
  }
  release(&fuse.lock);
}

// ---- /dev/fuse: the daemon's side ----------------------------------

void
fuse_attach(void)
{
  acquire(&fuse.lock);
  if(fuse.daemon_pid == 0)
    fuse.daemon_pid = myproc()->pid;
  release(&fuse.lock);
}

// Daemon read(): spin with yield() until a request is PENDING.
static int
fuse_dev_read(int user_dst, uint64 dst, int n)
{
  struct fuse_slot *s, *best;
  int i, pid = myproc()->pid;

  if(n < (int)sizeof(struct fuse_req))
    return -1;

  for(;;){
    acquire(&fuse.lock);
    if(fuse.daemon_pid != pid){
      release(&fuse.lock);
      return -1;
    }
    best = 0;
    for(i = 0; i < FUSE_NSLOT; i++){
      s = &fuse.slot[i];
      if(s->state == SL_PENDING &&
         (best == 0 || s->req.id < best->req.id))
        best = s;
    }
    if(best)
      break;
    if(killed(myproc())){
      release(&fuse.lock);
      return -1;
    }
    release(&fuse.lock);
    yield();              // wait for a caller to submit a request
  }

  best->state = SL_INFLIGHT;
  release(&fuse.lock);

  if(either_copyout(user_dst, dst, &best->req, sizeof(best->req)) < 0){
    acquire(&fuse.lock);
    if(best->req.op == FUSE_RELEASE){
      best->state = SL_FREE;
    } else {
      best->rep.err = -1;
      best->rep.len = 0;
      best->state   = SL_DONE;
    }
    release(&fuse.lock);
    return -1;
  }

  // RELEASE is one-way: free the slot immediately, no reply expected.
  if(best->req.op == FUSE_RELEASE){
    acquire(&fuse.lock);
    best->state = SL_FREE;
    release(&fuse.lock);
  }
  return sizeof(best->req);
}

// Daemon write(): match reply by id, store it, mark DONE so caller wakes.
static int
fuse_dev_write(int user_src, uint64 src, int n)
{
  struct fuse_rep r;
  struct fuse_slot *s = 0;
  int i;

  if(n < FUSE_REP_HDR || n > (int)sizeof(r))
    return -1;
  memset(&r, 0, sizeof(r));
  if(either_copyin(&r, user_src, src, n) < 0)
    return -1;
  if(r.err == 0 && (r.len < 0 || r.len > FUSE_DATAMAX))
    r.err = -1;

  acquire(&fuse.lock);
  for(i = 0; i < FUSE_NSLOT; i++){
    if((fuse.slot[i].state == SL_INFLIGHT ||
        fuse.slot[i].state == SL_ABANDON) &&
       fuse.slot[i].req.id == r.id){
      s = &fuse.slot[i];
      break;
    }
  }
  if(s == 0){
    release(&fuse.lock);
    return -1;            // unknown or duplicate reply
  }
  if(s->state == SL_ABANDON){
    s->state = SL_FREE;  // caller is gone; discard
  } else {
    memmove(&s->rep, &r, sizeof(r));
    s->state = SL_DONE;  // caller's yield loop will pick this up
  }
  release(&fuse.lock);
  return n;
}

// Called from exit(): fail all in-flight requests so callers don't
// spin forever after the daemon dies.
void
fuse_proc_exit(int pid)
{
  int i;
  struct fuse_slot *s;

  acquire(&fuse.lock);
  if(fuse.daemon_pid == pid){
    fuse.daemon_pid = 0;
    for(i = 0; i < FUSE_NSLOT; i++){
      s = &fuse.slot[i];
      if(s->state == SL_PENDING || s->state == SL_INFLIGHT){
        if(s->req.op == FUSE_RELEASE){
          s->state = SL_FREE;
        } else {
          s->rep.err = -1;
          s->rep.len = 0;
          s->state   = SL_DONE;
        }
      } else if(s->state == SL_ABANDON){
        s->state = SL_FREE;
      }
    }
  }
  release(&fuse.lock);
}

void
fuseinit(void)
{
  initlock(&fuse.lock, "fuse");
  devsw[FUSEDEV].read  = fuse_dev_read;
  devsw[FUSEDEV].write = fuse_dev_write;
}
