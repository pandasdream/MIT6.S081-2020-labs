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
#include "proc.h"
struct cpu;

#define BUCKETS 13
extern uint ticks;

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

} bcache;
struct spinlock bcache_lock[BUCKETS];
struct buf* hash_bucket[BUCKETS];
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < BUCKETS; i++)
    initlock(&bcache_lock[i], "bcache_lock");

  int i = 0;
  for(i = 0; i < BUCKETS; i ++)
    hash_bucket[i] = 0;
  i = 0;
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->blockno = i;
    b->timestamp = 0;
    b->next = hash_bucket[i];
    hash_bucket[i] = b;
    i = (i + 1) % BUCKETS;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int target = blockno % BUCKETS;
  int i = target;
  acquire(&bcache_lock[target]);

  // Is the block already cached?
  for(b = hash_bucket[target]; b; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt ++;
      release(&bcache_lock[target]);
      acquiresleep(&b->lock);
      // ! big bug
      // printf("hit and out\n");
      return b;
    }
  }
  // release(&bcache_lock[i]);        // too early, may cause remap, then refree, then crash

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf* lru_buf = 0;
  for(b = hash_bucket[i]; b; b = b->next) {
    if(b->dev == dev && b->refcnt == 0) {
      if(lru_buf == 0)
        lru_buf = b;
      else if(lru_buf->timestamp > b->timestamp)
        lru_buf = b;
    }
  }
  if(lru_buf)
    goto find;

  acquire(&bcache.lock);
  search:
  lru_buf = 0;
  for(i = 0; i < BUCKETS; i ++) {
    for(b = hash_bucket[i]; b != 0; b = b->next) {
      if(b->refcnt == 0) {
        if(lru_buf == 0)
          lru_buf = b;
        else if(lru_buf->timestamp > b->timestamp) {
          lru_buf = b;
        }
      }
    }
  }
  if(lru_buf == 0)
    panic("bget(): no buffers");
  // 不能用int source = ((lru_buf - bcache.buf) / sizeof(struct buf)) % BUCKETS;
  // 因为使用下标进行hash的做法只适用于初始化，之后的hash函数需要根据blockno进行hash
  int source = lru_buf->blockno % BUCKETS;
  if(source != target)
    acquire(&bcache_lock[source]);
  if(lru_buf->refcnt) {
    if(source != target)
      release(&bcache_lock[source]);
    goto search;
  }
  b = hash_bucket[source];
  while(b && b->next != lru_buf)
    b = b->next;
  if(b == 0)
    hash_bucket[source] = hash_bucket[source]->next;
  else
    b->next = lru_buf->next; 
  if(source != target)
    release(&bcache_lock[source]);
  lru_buf->next = hash_bucket[target];
  hash_bucket[target] = lru_buf;
  release(&bcache.lock);
  find:
  // why this acquiresleep never cause contend ?
  lru_buf->dev = dev;
  lru_buf->blockno = blockno;
  lru_buf->valid = 0;
  lru_buf->refcnt = 1;
  lru_buf->timestamp = ticks;

  // return a locked buf
  acquiresleep(&lru_buf->lock);
  for(int j = 0; j < 10000000; j ++) ;
  release(&bcache_lock[target]);
  return lru_buf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  // int i = ((b - bcache.buf) / sizeof(struct buf)) % BUCKETS;
  int i = b->blockno % BUCKETS;
  // printf("sss");
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  acquire(&bcache_lock[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->timestamp = ticks;
  }

  release(&bcache_lock[i]);
}

void
bpin(struct buf *b) {
  int i = b->blockno % BUCKETS;
  acquire(&bcache_lock[i]);
  b->refcnt++;
  release(&bcache_lock[i]);
}

void
bunpin(struct buf *b) {
  int i = b->blockno % BUCKETS;
  acquire(&bcache_lock[i]);
  b->refcnt--;
  release(&bcache_lock[i]);
}


