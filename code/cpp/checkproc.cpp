#include"_public.h"

//程序运行的日志
CLogFile logfile;

int main(int argc, char* argv[])
{
  //帮助文档
  if(argc != 2)
  {
    printf("Using:./checkproc logfilename\n");
    printf("Example:/canteen/code/bin/procctl 10 /canteen/code/bin/checkproc /canteen/log/checkproc.log\n\n");

    printf("本程序用于检查后台服务程序是否超时，如果超时，就终止他.\n");
    printf("注意：\n");
    printf(" 1）本程序由procctl启动，运行周期建议为10秒.\n");
    printf(" 2) 为了避免进程被普通用户误杀，本程序应由root用户启动。\n");
    printf(" 3）如果要终止本程序，只能用 kill -9 终止.\n\n\n");
    return -1;
  }
  
  //忽略全部信号和IO
  CloseIOAndSignal(true);  

  //打开程序的日志
  if(logfile.Open(argv[1],"a+")==false) 
  { printf("(logfile.Open(%s) failed.\n",argv[1]); return -1;  }

  //创建或获取共享内存
  int shmid=0;
  if( (shmid=shmget((key_t)SHMKEYP,MAXNUMP*sizeof(struct st_procinfo),0666|IPC_CREAT)) == -1)
  {
     logfile.Write("创建或获取共享内存（%x）失败",SHMKEYP);
     return -1;
  }
 
  //连接共享内存
  struct st_procinfo* shm = (struct st_procinfo*)shmat(shmid,0,0);   

  //遍历共享内存
  for(int ii=0 ; ii< MAXNUMP ; ii++)
  {
     //如果pid为0，表示空记录,continue
     if(shm[ii].pid == 0) { continue; }
     //如果pid不为0，此为服务程序的心跳信息
     //logfile.Write("pos=%d,pid=%d,pname=%s,timeout=%d,atime=%d\n",\
                                            ii,shm[ii].pid,shm[ii].pname,shm[ii].timeout,shm[ii].atime);
     
     //向服务程序发送信号0，判断它是否存在，如果不存在，删除该记录，continue
     int iret=kill(shm[ii].pid,0) ;
     if( iret==-1 )       
     {
       logfile.Write("进程pid=%d（%s）已经不存在.\n",shm[ii].pid,shm[ii].pname);
       memset(shm+ii,0,sizeof(struct st_procinfo));
       continue;
     }
 
     //如果未超时，continue;
     time_t now=time(0);
     if(now-shm[ii].atime < shm[ii].timeout) {continue;}
    
     //如果已超时
     logfile.Write("进程pid=%d(%s) 已经超时。\n",shm[ii].pid,shm[ii].pname);
     //发送信号15，尝试正常终止进程
     kill(shm[ii].pid,15);
     
     //每隔一秒判断一次进程是否存在，累计5秒，一般来说，5秒足够让进程退出  
     for(int jj=0 ; jj<5; jj++)
     {
        sleep(1);
        iret = kill(shm[ii].pid,0);
        if(iret==-1) { break; } 
     }
     //如果进程仍然存在，发送信号9强制终止它
     if(iret == -1) 
     {
        logfile.Write("进程pid=%d(%s) 已经正常终止。\n",shm[ii].pid,shm[ii].pname);   
     }
     else
     {
       kill(shm[ii].pid,9);
       logfile.Write("进程pid=%d(%s) 已经强制终止。\n",shm[ii].pid,shm[ii].pname);
     }
     //从共享内存中删除已经超时进程的心跳记录
     memset(shm+ii,0,sizeof(struct st_procinfo));    
  }

  //分离共享内存
  shmdt(shm);

  return 0;
}

