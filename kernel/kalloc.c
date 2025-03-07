// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct spinlock stealock;

struct {
  struct spinlock lock;
  // struct spinlock stealock;
  struct run *freelist;
} kmem[NCPU];



// static unsigned long rand_seed = 1;

// unsigned int
// random(void) {
//   rand_seed = rand_seed * 1664525 + 1013904223;  // LCG 公式
//   return (rand_seed >> 16) & 0x7FFF;  // 取低 15 位
// }

// uint64
// ticks_since_boot() {
//   return r_time();  // 读取当前 ticks
// }

// void
// delay_ticks(int ticks) {
//   uint64 start = ticks_since_boot();  // 读取当前时间
//   while (ticks_since_boot() - start < ticks) {
//     asm volatile("nop");  // 空操作，防止编译器优化掉
//   }
// }




void
kinit()
{

  // char lockname[8];
  // char stealockname[11];
  for(int i=0; i< NCPU;++i){
    // snprintf(lockname, sizeof(lockname), "kmem_%d", i);
    // snprintf(stealockname, sizeof(stealockname), "stealk_%d", i);
    initlock(&kmem[i].lock, "kmem_lock");
    // initlock(&kmem[i].stealock, stealockname);
  }
  initlock(&stealock, "stealock");

  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  int id = cpuid(); 
  push_off(); // 关闭中断

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 使用属于cpu的空闲链表锁，但是没有可分配内存的时候就需要借
void *
kalloc(void)
{
  struct run *r;
  int id = cpuid();
  push_off();
  acquire(&kmem[id].lock);

  r = kmem[id].freelist;
  if (r)
    kmem[id].freelist = r->next;
  else {
    int steal_id;
    // for (int i = 0; i < NCPU ; i++) {  
    //   steal_id = i; 
    //   if(steal_id == id) continue;

    //   // acquire(&stealock);

    //   acquire(&kmem[steal_id].lock);
    //   r = kmem[steal_id].freelist;
    //   if (r) {
    //     kmem[steal_id].freelist = r->next;
    //     release(&kmem[steal_id].lock);
    //     break;
    //   }
    //   release(&kmem[steal_id].lock);

    //   // release(&stealock);
    // }
    for (steal_id = id + 1; steal_id < NCPU; steal_id++) {
      acquire(&kmem[steal_id].lock);
      if (kmem[steal_id].freelist) {
          r = kmem[steal_id].freelist;
          kmem[steal_id].freelist = r->next;
          release(&kmem[steal_id].lock);
          break;
      }
      release(&kmem[steal_id].lock);
    }
      if (!r) {
        for (int steal_id = 0; steal_id < id; steal_id++) {
            if(steal_id == id) continue;
            acquire(&kmem[steal_id].lock);
            if (kmem[steal_id].freelist) {
                r = kmem[steal_id].freelist;
                kmem[steal_id].freelist = r->next;
                release(&kmem[steal_id].lock);
                break;
            }
            release(&kmem[steal_id].lock);
        }
    }
  
  }

  release(&kmem[id].lock);
  pop_off();

  if (r)
    memset((char*)r, 5, PGSIZE); // 填充 junk 数据
  return (void*)r;
}

