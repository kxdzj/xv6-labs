#include "kernel/types.h"
#include "user/user.h"

int main(int argc,char const *argv[]){
  if(argc != 1){
    fprintf(2, "usage: uptime\n");
    exit(1);
  }
  int nowticks = uptime();
  fprintf(1, "Uptime: %d ticks\n", nowticks);
  exit(0);
}