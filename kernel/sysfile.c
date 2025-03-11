//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


// 查找进程中包含虚拟地址的vma，并返回其指针
struct vma*
findvma(uint64 va){
  struct proc* p = myproc();

  for(int i=0;i<MAXVMANUM;++i){
    struct vma *vv = &p->vmas[i];
    if((vv->valid == 1) && (va >= vv->addr)  && (va < vv->addr + vv->len))
    {
      return vv;
    }
  }
  return 0;
}


// 在进程的地址空间中找到一个未使用的区域来映射文件，并将VMA添加到进程的映射区域表中
// void *mmap(void *addr, size_t length, int prot, int flags,
//   int fd, off_t offset);
// addr：建议的映射起始地址（可为 NULL，由内核决定）
// length：映射区域的大小（以字节为单位）
// prot：保护标志（如 PROT_READ、PROT_WRITE）
// flags：映射选项（如 MAP_SHARED、MAP_PRIVATE）
// fd：文件描述符，用于指定映射的文件
// offset：文件内容的起始偏移量
// 使用静态函数argfd同时解析文件描述符和struct file
uint64
sys_mmap(void){
  uint64 addr, err = 0xffffffffffffffff;
  int len, prot, flags, offset, fd;
  struct file*  vfile;

  if(argaddr(0, &addr) < 0 || argint(1, &len) <0 
      || argint(2,&prot) <0 || argint(3,&flags) < 0 
      || argfd(4, &fd, &vfile) < 0  || argint(5, &offset)  < 0 )
  {
    return err;
  }
  if( addr != 0 || offset != 0 || len < 0){
    return err;
  }
  // peot & PROT_READ 表示是否请求读
  // MAP_PRIVATE 允许创建私有映射，即写入不会影响原始文件，
  // 因此即使不能写，在 MAP_PRIVATE 模式下仍然可以允许映射
  if ((!vfile->readable && (prot & PROT_READ)) ||
  (((!vfile->writable) && (prot & PROT_WRITE)) && !(flags & MAP_PRIVATE))) {
  return err;
}

  
  len = PGROUNDUP(len);

  struct proc *p = myproc();
  struct vma *v =  0;
  uint64 vmaaddrend =  MMAPEND;
  
  for(int i=0;i<MAXVMANUM;++i){
    struct vma *vv = &p->vmas[i];
    if(vv->valid == 0){
      if(v == 0){
        v = &p->vmas[i];
        v->valid = 1;
      }
    }else if(vv->addr < vmaaddrend){ // 找使用的vma，确保未使用的vma的地址应该在这之下
      vmaaddrend = PGROUNDDOWN(vv->addr);
    }
  }

  if(v == 0) panic("mmap : no free vma!");
  
  v->addr = vmaaddrend - len;
  v->len  = len;
  v->prot = prot;
  v->vfile = vfile;
  v->flags = flags;
  v->offset = offset;
  
  // 增加文件引用计数
  filedup(v->vfile);
  
  return v->addr;
}



// int  munmap(void *addr, size_t length);
// addr 是指定需要解除映射的内存区域的起始地址，这个地址必须是之前通过 mmap 返回的地址
// 指定要解除映射的区域的长度（以字节为单位）
uint64
sys_munmap(void){
  uint64 addr, len;
  if( argaddr(0, &addr) < 0 || argaddr(1, &len) < 0 || len == 0){
    return -1;
  }
  struct proc* p = myproc();
  struct vma *v = findvma(addr);
  if(v == 0) {
    return -1;
  }
  if( addr > v->addr && addr + len < v->addr + v->len){
    return -1;
  }
  uint64 addr_aligned = addr;
  if(addr > v->addr){
    addr_aligned = PGROUNDUP(addr);
  }
  int nunmap = len - (addr_aligned - addr);
  if(nunmap < 0) nunmap = 0;

  vmaunmap(p->pagetable, addr_aligned, nunmap, v);


  if(addr <= v->addr && addr + len > v->addr){
    v->offset += addr +len - v->addr;
    v->addr = addr + len;
  }
  v->len -= len;

  if(v->len <= 0){
    fileclose(v->vfile);
    v->valid = 0;
  }

  return 0;
}
// uint64
// sys_munmap(void) {
//   uint64 addr;
//   int length;
//   if(argaddr(0, &addr) < 0 || argint(1, &length) < 0)
//     return -1;

//   int i;
//   struct proc* p = myproc();
//   for(i = 0; i < MAXVMANUM; ++i) {
//     if(p->vmas[i].valid && p->vmas[i].len >= length) {
//       // 根据提示，munmap的地址范围只能是
//       // 1. 起始位置
//       if(p->vmas[i].addr == addr) {
//         p->vmas[i].addr += length;
//         p->vmas[i].len -= length;
//         break;
//       }
//       // 2. 结束位置
//       if(addr + length == p->vmas[i].addr + p->vmas[i].len) {
//         p->vmas[i].len -= length;
//         break;
//       }
//     }
//   }
//   if(i == MAXVMANUM)
//     return -1;

//   // 将MAP_SHARED页面写回文件系统
//   if(p->vmas[i].flags == MAP_SHARED && (p->vmas[i].prot & PROT_WRITE) != 0) {
//     filewrite(p->vmas[i].vfile, addr, length);
//   }

//   // 判断此页面是否存在映射
//   uvmunmap(p->pagetable, addr, length / PGSIZE, 1);


//   // 当前VMA中全部映射都被取消
//   if(p->vmas[i].len == 0) {
//     fileclose(p->vmas[i].vfile);
//     p->vmas[i].valid = 0;
//   }

//   return 0;
// }


// 按懒加载需要分配物理页并且映射，需要懒加载且成功返回0，否则-1
int
vmatraylazy_touch(uint64 va){
 
  struct proc* p = myproc();
  struct vma* v = findvma(va);
  
  if(v==0){
    return -1;
  }
  // 分配一个物理页
  void* pa = kalloc();
  if(pa == 0){
    panic("vmatraylazy_touch failed! cause : kalloc failed!");
    return -1;
  }
  
  memset(pa, 0, PGSIZE);
  // 开始文件系统操作
  begin_op();

  // 获取文件的锁
  ilock(v->vfile->ip);
  // readi() 读取文件数据
  
  readi(v->vfile->ip, 0, (uint64)pa, v->offset + PGROUNDDOWN(va - v->addr), PGSIZE);
  
  iunlock(v->vfile->ip);
  end_op();

  int perm = PTE_U;
  if( v->prot & PROT_READ) perm |= PTE_R;
  if( v->prot & PROT_WRITE) perm |= PTE_W;
  if( v->prot & PROT_EXEC)  perm |= PTE_X;
  
  if(mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) < 0)
  {
    kfree(pa);
    panic("vmatraplazy_touch failed!");
    return -1;
  }
  return 0;
}

