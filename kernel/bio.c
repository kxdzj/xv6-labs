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

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

#define NBUCKET 13
#define HASH(id)    (id % NBUCKET)


struct hashbuf {
  struct buf head;
  struct spinlock lock;
};

struct {
  struct buf buf[NBUF];
  struct hashbuf buckets[NBUCKET]; // 缓存哈希桶
  struct spinlock get_lock; // 分配锁
}bcache;


void
binit(void)
{
  struct buf *b;
  // char lockname[16];

  for(int i=0; i<NBUCKET; ++i){
    // snprintf(lockname, sizeof(lockname), "bcache_%d", i);
    initlock(&bcache.buckets[i].lock, "bcache");

    // 初始化缓存哈希桶的头节点
    bcache.buckets[i].head.prev  = &bcache.buckets[i].head;
    bcache.buckets[i].head.next  = &bcache.buckets[i].head;
  }
 
  // 初始化缓冲区列表
  int start = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->lasttime = 0;
    b->refcnt = 0;
    b->next = bcache.buckets[start].head.next;
    b->prev = &bcache.buckets[start].head;
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[start].head.next->prev = b;
    bcache.buckets[start].head.next = b;
    start = (start+1)%NBUCKET;
  }
  // 初始化分配锁
  initlock(&bcache.get_lock, "bcache_getlock");

}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int key = HASH(blockno);

  acquire(&bcache.buckets[key].lock);

  // Is the block already cached?
  for(b = bcache.buckets[key].head.next; b != &bcache.buckets[key].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->lasttime = ticks;

      release(&bcache.buckets[key].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 未命中
  release(&bcache.buckets[key].lock);
  acquire(&bcache.get_lock);


  // 需要再检测是否为空
  for(b = bcache.buckets[key].head.next; b != &bcache.buckets[key].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      // 会改变refcnt 和 lasttime 所以需要加锁
      acquire(&bcache.buckets[key].lock);
      b->refcnt++;
      b->lasttime = ticks;

      release(&bcache.buckets[key].lock);
      release(&bcache.get_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 经过检测还是为空。证明拿到锁的进程确实是第一个为这个缓存分配的
  //  遍历所有哈希桶，寻找 refcnt == 0 且 lastuse 最小的缓存块（LRU）
  // 始终只持有一个桶的锁，避免死锁
  // 如果找不到合适的缓存块，系统 panic 退出
  // 最终返回 before_least->next，即真正的 LRU 缓存块
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf *least = 0;
  int holding_bucket = -1;
  for(int i=0;i < NBUCKET; ++i){ // 这个流程走完就会找到块

      acquire(&bcache.buckets[i].lock);
      int newfound = 0;
      for(b = bcache.buckets[i].head.next; b != &bcache.buckets[i].head; b = b->next){
          if(b->refcnt == 0 && ((!least) || b->lasttime < least->lasttime))
          {
              least = b;
              newfound = 1;
          }
      }
      if(!newfound){ // 这个桶没找到，那就释放锁
        release(&bcache.buckets[i].lock);
      }else{ // 找到了，那就先标记一下，但是要注意是不是拿了其他桶的锁
        // 这种情况，那就是你找到了3桶有空闲块，再去找5桶的时候需要先释放锁
        if(holding_bucket != -1) release(&bcache.buckets[holding_bucket].lock);
        holding_bucket = i;
      }
  }
  if(!least){
    panic("bget: no buffers!");
  }
  b = least; // b是我们找到的空闲块

  if(holding_bucket != key){

    least->prev->next = least->next;
    least->next->prev = least->prev;
    release(&bcache.buckets[holding_bucket].lock);
    // 上面操作是从源同拿出并释放锁
    acquire(&bcache.buckets[key].lock);
    b->next = bcache.buckets[key].head.next;
    b->prev = &bcache.buckets[key].head;
    bcache.buckets[key].head.next->prev = b;
    bcache.buckets[key].head.next = b;
  }
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  b->lasttime = ticks;
  release(&bcache.buckets[key].lock);
  release(&bcache.get_lock);

  acquiresleep(&b->lock);
  return b;
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
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  int key = HASH(b->blockno);

  acquire(&bcache.buckets[key].lock);

  b->refcnt--;
  if (b->refcnt == 0) {
    b->lasttime = ticks;
  }
  
  release(&bcache.buckets[key].lock);
}

void
bpin(struct buf *b) {
  int key = HASH(b->blockno);
  acquire(&bcache.buckets[key].lock);
  b->refcnt++;
  release(&bcache.buckets[key].lock);
}

void
bunpin(struct buf *b) {
  int key = HASH(b->blockno);
  acquire(&bcache.buckets[key].lock);
  b->refcnt--;
  release(&bcache.buckets[key].lock);
}


