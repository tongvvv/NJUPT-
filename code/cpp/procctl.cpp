#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<sys/types.h>
#include<sys/wait.h>

int main(int argc, char* argv[])
{
  if(argc<3)
  {
     printf("Using:./procctl timeval program argv  ... \n");
     printf("Example: /canteen/code/bin/procctl 5 /usr/bin/tar  zcvf /tmp/tmp.tgz /usr/include");

     printf("本程序是服务程序的调度程序，周期性启动服务程序或shell脚本。\n");
     printf("timeval 运行周期，单位：秒。被调度的程序运行结束后，在timeval秒后会被procctl重新启动。\n");
     printf("program 被调度的程序名，必须使用全路径。\n");
     printf("argv    被调度的程序的参数。\n");
     printf("注意：本程序不会被kill杀死，但可以用kill -9 强行杀死。\n\n");
     return -1;
  }  
  
  char *pargv[argc];

  memset(pargv,0,sizeof(pargv));

  for(int ii=2; ii<argc ; ii++)
  {
     pargv[ii-2] = argv[ii] ;
  }
  
  for(int ii=0; ii<64 ; ii++)
  {
     signal(ii,SIG_IGN); close(ii); //忽略信号，关闭IO
  }
  //让父进程退出，子进程在后台运行，交给1号进程托管
  if(fork() != 0 ) exit(0);
  
  //恢复子进程结束信号，让父进程可以wait子进程结束的状态；
  signal(SIGCHLD,SIG_DFL);

  while(true)
  {
     if(fork() == 0)
     {
        execv(argv[2],pargv);
        exit(0); //如果上面execv执行成功，exit不会执行;如果execv执行失败，那么执行exit防止产生过多无效进程
     }
     else
     {
        int status;
        wait(&status); //被调度的程序退出之后，wait捕捉到子进程退出的状态，那么休息timeval秒后重新启动他
        sleep(atoi(argv[1]));
     }
  }
  return 0;
}
