#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/param.h"

void exe(char * program, char **args){
  if(fork() == 0){// child process
    exec(program, args);
    exit(0);
  }
  return;
}

int main(int argc, char *argv[]){
    char buf[1024];
    char *p = buf, *start_p = buf; // 结束和开始指针
    char *argsbuf[MAXARG]; // #define MAXARG 32
    char **args = argsbuf; // 指向参数缓存数组的第一个标准输入参数
    for(int i=1; i<argc; ++i){
       *(args++) = argv[i];
    }
    char **pa = args; 
    while(read(0, p, 1) != 0){
      if(*p == ' ' || *p == '\n'){ //读入参数完毕
         *p = '\0';
         *(pa++) = start_p;
         start_p = p+1; // 从下一个位置开始，这是下一个参数的开始
        if(*p == '\n'){
          *pa = '\0';
          exe(argv[1], argsbuf);
          pa = args; // 重置为第一个标准输入的空位指针
        }
      }
      p++; // 读入一个字符就往下走
    }
    if(pa != args){// 最后一行没有换行符
      *p = '\0';
      *(pa++) = start_p;

      *pa = '\0'; //给argsbuf上末尾标记
      exe(argv[1], argsbuf);

    }
  while (wait(0) != -1)
  {
    // 循环等待所有子进程完成
  }
  exit(0);
}