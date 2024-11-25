#include "kernel/types.h"
#include "user/user.h"

#define  RD  0
#define  WR  1

int main(int argc, char const *argv[]){
    if(argc != 1){
      fprintf(2, "usage:pingpong\n");
      exit(1);
    }
    // 创建两个管道 
    int p_c[2];
    int c_p[2];

    char buf= 'C';

    pipe(p_c);
    pipe(c_p);

    int pid = fork();
    int exitstatus = 0;

    if(pid < 0){
      close(p_c[RD]);
      close(p_c[WR]);
      close(c_p[RD]);
      close(c_p[WR]);
      fprintf(2, "fork error\n");
      exit(1);
    }
    else if(pid == 0){ // 子进程
      close(p_c[WR]);
      close(c_p[RD]);


      if(write(c_p[WR], &buf, sizeof(char)) != sizeof(char)){
        fprintf(2, "child write() error\n");
        exitstatus = 1;
      }

      if(read(p_c[RD], &buf, sizeof(char)) != sizeof(char)){
        fprintf(2, "child read() error\n");
        exitstatus = 1;
      }else{
        fprintf(1, "%d: received pong\n", getpid());
      }

      
      close(p_c[RD]);
      close(c_p[WR]);

      exit(exitstatus);
    }
    else{ // 父进程
      close(p_c[RD]);
      close(c_p[WR]);

      
        
      if(read(c_p[RD], &buf, sizeof(char)) != sizeof(char)){
        fprintf(2, "parent read() error\n");
        exitstatus = 1;
      }else{
        fprintf(1, "%d: received ping\n", getpid());
      }

      if(write(p_c[WR], &buf, sizeof(char)) != sizeof(char)){
        fprintf(2, "parent write() error\n");
        exitstatus = 1;
      }
      

      close(c_p[RD]);
      close(p_c[WR]);
      wait(0);

      exit(exitstatus);

    }
    exit(exitstatus);
}