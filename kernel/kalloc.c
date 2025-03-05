// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
// #define ref_index(pa)  (((uint64_t(pa)) - KERNBASE) / PGSIZE)


#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.


// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
/*
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)
*/


// 定义一个用于管理页面计数的数组，需要锁
struct ref {
  struct spinlock lock;
  // 128*1024*1024/ 4096 = 32768
  int count[PHYSTOP/PGSIZE]; // 引用计数
} ref;



struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref.lock, "ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    { 
      acquire(&ref.lock);
      ref.count[(uint64)p  / PGSIZE] = 1; // 因为kfree会先-1，所以初始化需要设为1
      release(&ref.lock);
      kfree(p);
    }
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

  acquire(&ref.lock);
  if(--ref.count[(uint64)pa / PGSIZE] == 0 ){ // 减少计数并判断
    release(&ref.lock);

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }else{
    release(&ref.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    acquire(&ref.lock);
    ref.count[(uint64)r  / PGSIZE] = 1;  // 引用初始化为1
    release(&ref.lock);
  }
    
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// 获取物理地址所在内存页面的使用计数
int
get_refcount(void* pa){
  return ref.count[(uint64)pa / PGSIZE ];
}

// 增加内存页面的使用计数
// 地址需要对齐再传入
int
add_refcount(void* pa){
  //  需要先判断地址合不合法 是不是页首地址 是不是内核数据段 是不是超出物理内存范围
  if(((uint64)pa % PGSIZE != 0) || ((char*)pa < end) || ((uint64)pa >= PHYSTOP)){
      printf("addr wrong!\n");
      return -1;
  }
  acquire(&ref.lock);
  ++ref.count[(uint64)pa / PGSIZE ];
  release(&ref.lock);
  return 0;

}


// 虚拟地址对应的物理地址是否在一个COW页面 是就返回1，否则 0
int
Is_cowpage(uint64 va){
  if( va >= MAXVA) // 超出最大虚拟地址
  {
    return 0;
  }
  struct proc *p = myproc();
  pte_t* pte = walk(p->pagetable, va, 0);
  
  return ( va < p->sz ) 
    && ( pte != 0 )
    && ( *pte & PTE_V )
    && ( *pte & PTE_COW );

}

// 当试图写入一个COW页，就需要实际复制一个懒复制页，并且重新映射为可写，其中0成功，-1失败
int
cow_alloc(uint64 va){
  struct proc *p = myproc();
  // 虚拟地址未对齐
  // if( va % PGSIZE != 0) 
  // {
  //   printf("bug here!");
  //   return -1;
  // }  
  uint64 pa = walkaddr(p->pagetable, va);
  if(pa == 0)  return -1;

  pte_t* pte = walk(p->pagetable, va, 0);

  if(get_refcount((char*)pa) == 1){ // 只有1个计数，改变权限即可
    *pte |= PTE_W;
    *pte &= ~PTE_COW;
    return 0;
  }

  else{
    char* mem = kalloc();
    if(mem == 0)  return -1;
    memmove(mem, (char*)pa, PGSIZE);
    // 需要清楚PTE_V标志位，才可以正常映射va到mem，之后恢复
    *pte &= ~PTE_V;
    if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, 
    (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW)  != 0)
    { // 映射失败，返回状态
      kfree(mem);
      *pte |= PTE_V;
      return -1;
    }

    kfree((char*)PGROUNDDOWN(pa));
    // 这里会减少原来COW页面的计数
    return 0;
  }

}