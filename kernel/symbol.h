#ifndef SYMBOLS_H
#define SYMBOLS_H

#define MAX_SYMBOLS 1024  // 最多存 1024 个符号
#define MAX_NAME_LEN 64   // 符号名最大长度

struct symbol {
    uint64 addr;
    char name[MAX_NAME_LEN];
};

extern struct symbol symbols[MAX_SYMBOLS];
extern int symbol_count;

void load_symbols();
char* find_symbol(uint64 addr);

#endif
