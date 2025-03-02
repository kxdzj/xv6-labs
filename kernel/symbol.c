#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "proc.h"
#include "file.h"
#include "defs.h"
#include "symbol.h"




int readline(struct file *f, char *buf, int max) {
    int i = 0;
    char c;
    while (i < max - 1) {
        if (readi(f->ip, 0, (uint64)&c, f->off, 1) != 1)
            break;  // 读不到数据
        f->off++;    // 移动文件偏移
        if (c == '\n') 
            break;  // 读到换行符
        buf[i++] = c;
    }
    buf[i] = '\0';  // 终止字符串
    return i;
}


char* strtok(char *str, const char *delim) {
    static char *next = 0;
    if (str)
        next = str;
    if (!next)
        return 0;
    
    // 跳过前导分隔符
    while (*next && strchr(delim, *next)) next++;
    if (!*next)
        return 0;
    
    // 找到 token 开头
    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    
    if (*next) {
        *next = '\0';
        next++;
    }
    
    return start;
}

uint64 strtoull(const char *s, int base) {
    uint64 val = 0;
    while (*s) {
        char c = *s++;
        int digit = 0;

        if (c >= '0' && c <= '9') 
            digit = c - '0';
        else if (c >= 'a' && c <= 'f') 
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') 
            digit = c - 'A' + 10;
        else 
            break;

        val = val * base + digit;
    }
    return val;
}


/**
 * 读取 `kernel.sym` 并存入 `symbols[]` 数组
 */
 struct symbol symbols[MAX_SYMBOLS];
 int symbol_count = 0;
 
 void load_symbols() {
     struct inode *ip;
     struct file f;
     char buf[128];
 
     begin_op();
     ip = namei("kernel.sym");
     if (ip == 0) {
         end_op();
         return;
     }
     ilock(ip);
     
     f.ip = ip;
     f.off = 0;
     f.type = FD_INODE;
     
     int i = 0;
     while (i < MAX_SYMBOLS && readline(&f, buf, sizeof(buf)) > 0) {
         char *addr_str = strtok(buf, " ");
         char *name = strtok(0, " ");
         if (addr_str && name) {
             symbols[i].addr = strtoull(addr_str, 16);
             strncpy(symbols[i].name, name, sizeof(symbols[i].name) - 1);
             symbols[i].name[sizeof(symbols[i].name) - 1] = '\0';
             i++;
         }
     }
     
     symbol_count = i;
     iunlockput(ip);
     end_op();
 }
/**
 * 根据 `addr` 查找对应的函数名
 */
const char* find_symbol(uint64 addr) {
    for (int i = 0; i < symbol_count; i++) {
        if (symbols[i].addr == addr)
            return symbols[i].name;
    }
    return "<unknown>";
}
