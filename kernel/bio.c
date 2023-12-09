// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// Add buckets
#define NBUCKET 13
#define HASH(x) (((uint)(x)) % NBUCKET)

struct
{
  struct spinlock lock[NBUCKET]; // array of locks for buckets
  struct buf buf[NBUF];
  struct spinlock master;   // lock for whole bcache
  struct buf head[NBUCKET]; // array of heads for each bucket one head
} bcache;

void binit(void)
{
  struct buf *b;

  initlock(&bcache.master, "bcache_master");

  for (int i = 0; i < NBUCKET; i++)
  {
    // For each BUCKET create head of linked list
    initlock(&bcache.lock[i], "bcache_bucket");
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }
  // For each buffer
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  int h = HASH(blockno);
  acquire(&bcache.lock[h]);

  // Is the block already cached?
  for (b = bcache.head[h].next; b != &bcache.head[h]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.lock[h]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  acquire(&bcache.master);

  for (b = bcache.buf; b != bcache.buf + NBUF; b++)
  {
    // find free buff
    int current_h = HASH(b->blockno);
    // find free buff in your own hash
    if (current_h == h)
    {
      if (b->refcnt == 0)
      {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        release(&bcache.master);
        release(&bcache.lock[h]);
        acquiresleep(&b->lock);
        return b;
      }
      continue;
    }
    // find free buff in other buckets
    acquire(&bcache.lock[current_h]);
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      // insert found buff to "h" bucket
      b->prev->next = b->next;
      b->next->prev = b->prev;
      b->next = bcache.head[h].next;
      b->prev = &bcache.head[h];
      bcache.head[h].next->prev = b;
      bcache.head[h].next = b;

      release(&bcache.lock[current_h]);
      release(&bcache.lock[h]);
      release(&bcache.master);
      acquiresleep(&b->lock);
      return b;
    }
    release(&bcache.lock[current_h]);
  }
  release(&bcache.master);
  release(&bcache.lock[h]);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int h = HASH(b->blockno);
  acquire(&bcache.lock[h]);
  b->refcnt--;
  release(&bcache.lock[h]);
}


void bpin(struct buf *b)
{
  int h = HASH(b->blockno);
  acquire(&bcache.lock[h]);
  b->refcnt++;
  release(&bcache.lock[h]);
}

void bunpin(struct buf *b)
{
  int h = HASH(b->blockno);
  acquire(&bcache.lock[h]);
  b->refcnt--;
  release(&bcache.lock[h]);
}