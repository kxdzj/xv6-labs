#include "kernel/types.h"
#include "user/user.h"

#define INT_LEN sizeof(int)
#define MAX_NUM 35
#define RD 0
#define WR 1

void sieve_primes(int read_fd){
  int prime;
  if(read(read_fd, &prime, INT_LEN) > 0){
    fprintf(1, "prime %d\n", prime);
    int newfd[2];
    pipe(newfd);
    int pid = fork();

    if(pid == 0){ // 子进程
    close(newfd[WR]);
    sieve_primes(newfd[RD]);
    close(newfd[RD]);
    exit(0);
    }
    else{ // 父进程
    close(newfd[RD]);
    int num;
    while(read(read_fd, &num, INT_LEN) > 0){
      if(num%prime)
      write(newfd[WR], &num, INT_LEN);
    }
    close(newfd[WR]);
    wait(0); // 等待子进程结束
    }
  }
}

int main(int argc, char const* argv[]){
  if(argc != 1){
    fprintf(2, "usage:primes\n");
    exit(1);
  }
  int pipe_fd[2];
  pipe(pipe_fd);
  // 传入数据
  for(int i=2;i <= MAX_NUM;++i){
    write(pipe_fd[WR], &i, INT_LEN);
  }
  close(pipe_fd[WR]);
  int pid = fork();
  if(pid == 0){
    sieve_primes(pipe_fd[RD]);
    close(pipe_fd[RD]);
    exit(0);
  }
  else{
    close(pipe_fd[RD]);
    wait(0);
  }
  exit(0);
}