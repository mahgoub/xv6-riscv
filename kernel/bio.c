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
#include "hashmap.h"  // Hypothetical hash map implementation for fast lookups

#define DEFAULT_NBUF 128

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf head;
  struct hashmap *cache_map;  // Hash map for fast buffer lookups
  int cache_size;
} bcache;

// Initialize the buffer cache
void binit(void) {
  struct buf *b;

  // Initialize global lock for the cache
  initlock(&bcache.lock, "bcache");

  // Initialize the LRU linked list and hash map
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  bcache.cache_map = hashmap_create();
  bcache.cache_size = DEFAULT_NBUF;

  // Create linked list of buffers and add to hash map
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
    hashmap_put(bcache.cache_map, b->dev, b->blockno, b);  // Add buffer to hash map
  }
}

// Get a cached buffer or allocate a new one
static struct buf* bget(uint dev, uint blockno) {
  struct buf *b;

  acquire(&bcache.lock);

  // Try to find the buffer in the cache
  b = hashmap_get(bcache.cache_map, dev, blockno);
  if (b != NULL) {
    b->refcnt++;
    release(&bcache.lock);
    acquiresleep(&b->lock);
    return b;
  }

  // Buffer not cached; recycle least recently used (LRU) buffer
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      hashmap_put(bcache.cache_map, dev, blockno, b);  // Add new buffer to cache
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No available buffer; retry or put the process to sleep
  release(&bcache.lock);
  panic("bget: no buffers");
}

// Read the contents of a block into a buffer
struct buf* bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);  // Read from disk
    b->valid = 1;
  }
  return b;
}

// Write buffer contents to disk
void bwrite(struct buf *b) {
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);  // Write to disk
}

// Release a buffer and move it to the head of the LRU list
void brelse(struct buf *b) {
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // No one is using it, move to head of the LRU list
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

// Pin the buffer in cache to prevent eviction
void bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

// Unpin the buffer and allow eviction
void bunpin(struct buf *b) {
  acquire(&bcache.lock);
  if (b->refcnt <= 0) {
    panic("bunpin: refcnt already zero");
  }
  b->refcnt--;
  release(&bcache.lock);
}

// Optionally set cache size dynamically
void set_cache_size(int size) {
  acquire(&bcache.lock);
  bcache.cache_size = size;
  release(&bcache.lock);
}

