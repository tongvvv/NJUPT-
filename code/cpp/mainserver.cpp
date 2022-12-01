#include "_public.h"
#include "connpool.h"
#define DEFAULT_PORT 5005
#define MAXSOCK 50000
#define MAXTHREAD  10
#define MAXCONN    20

CLogFile logfile;
CPActive PActive;

struct st_pthinfo
{
  pthread_t pthid; //进程号
  time_t    atime; //最近一次活动时间
};
vector<struct st_pthinfo> vthid; // 存放全部线程id的容器。
pthread_mutex_t vthidlock=PTHREAD_MUTEX_INITIALIZER;    // vthid容器的锁

//记录所有连上来的sock的身份，0未登录，1管理员，2店铺，3学生
//映射关系为 socket ------>  type_id
struct type_id
{
  int type;
  int id;
};

map<int,struct type_id> socktype;
pthread_mutex_t mutex_sock_type = PTHREAD_MUTEX_INITIALIZER; //map的锁

//店铺登录上来的时候把id填进去，为了方便菜品浏览购买功能
//映射关系为 shopid ------> socket
map<int,int> shopid_sock;
pthread_mutex_t mutex_shopid_sock = PTHREAD_MUTEX_INITIALIZER; //map的锁


struct st_msg
{
  int sock;   //来消息的sock
  char buffer[2048]; //消息
};

deque<struct st_msg> msgqueue;                          //sock消息队列
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;  //缓存队列的锁
pthread_cond_t  cond =PTHREAD_COND_INITIALIZER;   //线程池条件变量

// 初始化服务端的监听端口。
int initserver(int port);

int epollfd=0; //epoll句柄
int tfd = 0;   //定时器句柄

int listensock;

void EXIT(int sig);

//检查数据库连接池的线程主函数
void* checkpool(void* arg);
// 监控数据库连接池的子线程的id
pthread_t checkpoolid;  
//数据库连接池对象
connpool orapool;

//在注册功能中当向表中插入数据时，先用序列生成器nextval得到值并插入，然后currval得到刚才插入的id
//当多个连接并发insert时， nextval和currval会出现数据不一致
//所以接下来锁仅仅只在插入表的时候会用到
//流程为:  先insert(nextval)   再select(currval)
//其实也就是说  向表里插入数据得一个一个来，不能一起，这种方案也许会造成一些性能损耗，这里暂且先这样
pthread_mutex_t T_CARD = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t T_SHOP = PTHREAD_MUTEX_INITIALIZER;

pthread_t checkpthid;            // 监控线程主函数的子线程的id
void* checkthmain(void* arg);    // 监控线程主函数
void* thmain(void *arg);         // 线程主函数。
void thcleanup(void *arg);       // 线程清理函数。


int main(int argc,char *argv[])
{
  if (argc != 2) 
  {
     printf("\n");
     printf("Using: ./mainserver logfile \n\n");
     printf("Example: ./mainserver /canteen/log/mainserver.log \n\n");
     printf("	      /canteen/code/bin/procctl 5 /canteen/code/bin/mainserver /canteen/log/mainserver.log\n\n");

     return -1;
  }

  //关闭I0和信号，设置本程序的信号
  CloseIOAndSignal(true);  signal(2,EXIT); signal(15,EXIT);

  if(logfile.Open(argv[1],"a+")==false)
  {
    printf("logfile.Open(%s) failed.\n",argv[1]); return -1;
  }
  
  //PActive.AddPInfo(60,"mainserver"); //设置进程的超时时间60秒

  //初始化数据库连接池
  if(orapool.init("c##tw/twidc@tw_oracle","Simplified Chinese_China.AL32UTF8",MAXCONN,50) == false)
  {
    logfile.Write("oraconnpool.init() failed.\n"); return -1;
  }

  //创建数据库连接池监控线程
  if (pthread_create(&checkpoolid,NULL,checkpool,NULL)!=0)
  {
    logfile.Write("pthread_create() failed.\n"); return -1;
  }

  //启用10个工作线程，线程比cpu核数略多
  for(int ii=0; ii<MAXTHREAD ; ii++)
  {
    struct st_pthinfo stpthinfo;
    if (pthread_create(&stpthinfo.pthid,NULL,thmain,(void *)(long)ii)!=0)
    {
       logfile.Write("pthread_create() failed.\n"); EXIT(-1);
    }

    stpthinfo.atime=time(0);
    vthid.push_back(stpthinfo);    // 把线程id放入容器。 
  }

  //创建子线程监控线程
  if(pthread_create(&checkpthid,NULL,checkthmain,NULL)!=0)
  {
     logfile.Write("pthread_create() failed.\n"); EXIT(-1);
  }

  //初始化监听socket
  listensock = initserver(DEFAULT_PORT);
  //把socket设置为非阻塞模式
  fcntl(listensock,F_SETFL,fcntl(listensock,F_GETFL,0)|O_NONBLOCK);

  // 创建epoll句柄。
  epollfd=epoll_create(1);

  struct epoll_event ev;  // 声明事件的数据结构。

  // 为监听的socket准备可读事件。
  ev.events=EPOLLIN;      // 读事件。
  ev.data.fd=listensock;  // 指定事件的自定义数据，会随着epoll_wait()返回的事件一并返回。
  epoll_ctl(epollfd,EPOLL_CTL_ADD,listensock,&ev); // 把监听的socket的事件加入epollfd中。

  //创建定时器
  tfd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);

  struct itimerspec timeout;
  memset(&timeout,0,sizeof(struct itimerspec));
  timeout.it_value.tv_sec=20; //超时时间为20秒
  timeout.it_value.tv_nsec=0;
  //timerfd_settime(tfd,0,&timeout,NULL);

  //为定时器准备事件
  ev.data.fd=tfd;
  ev.events=EPOLLIN|EPOLLET;  //读事件，并且一定要ET模式  
  //epoll_ctl(epollfd,EPOLL_CTL_ADD,tfd,&ev);

  struct epoll_event evs[10];      // 存放epoll返回的事件。

  while (true)
  {
    // 等待监视的socket有事件发生。
    int infds=epoll_wait(epollfd,evs,10,-1);

    // 返回失败。
    if (infds < 0)
    {
      perror("epoll() failed"); break;
    }

    // 如果infds>0，表示有事件发生的socket的数量。
    // 遍历epoll返回的已发生事件的数组evs。
    for (int ii=0;ii<infds;ii++)
    {
      /////////////////////////////////////////
      //如果定时器的时间已到,设置进程心跳,清理空闲的客户端socket
      
      if(evs[ii].data.fd == tfd)
      {
        //timerfd_settime(tfd,0,&timeout,NULL); //重新设置定时器

        //PActive.UptATime();

        continue;
      }
      //////////////////////////////////////////////////////////////
      // 如果发生事件的是listensock，表示有新的客户端连上来。
      else if (evs[ii].data.fd == listensock)
      {
	//接收客户端的连接
	struct sockaddr_in client;
	socklen_t len = sizeof(client);
	int srcsock = accept(listensock,(struct sockaddr*)&client,&len);
	if(srcsock < 0) { continue;}
	if(srcsock >= MAXSOCK)
	{
	  logfile.Write("连接数已超过最大值%d.\n",MAXSOCK); close(srcsock); continue;
	}

	//加入epoll中
        ev.events=EPOLLIN;     
        ev.data.fd=srcsock;  
        epoll_ctl(epollfd,EPOLL_CTL_ADD,srcsock,&ev); 

	//设置为非阻塞模式,这里不用非阻塞模式了 降低点编程难度
	//fcntl(srcsock,F_SETFL,fcntl(srcsock,F_GETFL,0)|O_NONBLOCK);
	
	//把它放进socktype里面，记录下来
	struct type_id tpid;
	tpid.type = 0; tpid.id = 0;
	pthread_mutex_lock(&mutex_sock_type);
	socktype[srcsock] = tpid;
	pthread_mutex_unlock(&mutex_sock_type);
	continue;
      }
      else
      {
	// 如果是客户端连接的socke有事件，表示有报文发过来或者连接已断开。
	struct st_msg stmsg;
	stmsg.sock = evs[ii].data.fd;
	memset(stmsg.buffer,0,sizeof(stmsg.buffer));
	//如果连接断开了
	if(Read(stmsg.sock,stmsg.buffer,1) != true)
	{
	  pthread_mutex_lock(&mutex_sock_type);
	  //找到socktype中对应的sock
	  auto it =  socktype.find(stmsg.sock);
	  //如果是店铺，从shopid_sock中删去
	  if((it->second).type == 2)
	  {
	    pthread_mutex_lock(&mutex_shopid_sock);
	    auto iter = shopid_sock.find((it->second).id); 
	    shopid_sock.erase(iter);
	    pthread_mutex_unlock(&mutex_shopid_sock);
	  }
	  //从总的socktype中删去
	  socktype.erase(it);
	  pthread_mutex_unlock(&mutex_sock_type);
	  close(stmsg.sock);
	  continue;
        }
	else
	{
	  //有消息发过来，放入缓存队列交给线程池去处理
	  //一定要注意，此时buffer里的内容是gbk编码，线程主函数要自己去转换编码
	  pthread_mutex_lock(&mutex);
	  msgqueue.push_front(stmsg);
	  pthread_mutex_unlock(&mutex);
          pthread_cond_broadcast(&cond);
	  continue;
	}
      }
    }
  }

  return 0;
}

void EXIT(int sig)
{
  // 以下代码是为了防止信号处理函数在执行的过程中被信号中断。
  signal(SIGINT,SIG_IGN); signal(SIGTERM,SIG_IGN);

  logfile.Write("进程退出，sig=%d。\n",sig);

  close(listensock);

  //线程退出的时候会把自身id从容器中删除，size大小会改变，所以这里必须加锁
  pthread_mutex_lock(&vthidlock);
  // 取消全部的线程。
  for (int ii=0;ii<vthid.size();ii++)
  {
    pthread_cancel(vthid[ii].pthid);
  }
  pthread_mutex_unlock(&vthidlock);

  sleep(1);        // 让子线程有足够的时间退出。

  pthread_cancel(checkpthid);  //取消监控线程
  pthread_cancel(checkpoolid); //取消监控数据库连接池的子线程

  pthread_mutex_destroy(&vthidlock);
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);

  close(epollfd);
  close(tfd);

  exit(0);
}

// 初始化服务端的监听端口。
int initserver(int port)
{
  int sock = socket(AF_INET,SOCK_STREAM,0);
  if (sock < 0)
  {
    perror("socket() failed"); return -1;
  }

  int opt = 1; unsigned int len = sizeof(opt);
  setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&opt,len);

  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port);

  if (bind(sock,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0 )
  {
    perror("bind() failed"); close(sock); return -1;
  }

  if (listen(sock,20) != 0 )
  {
    perror("listen() failed"); close(sock); return -1;
  }

  return sock;
}

// 检查数据库连接池的线程函数
void* checkpool(void* arg)
{
  while(true)
  {
    orapool.checkpool();
    sleep(30);
  }
}

// 监控线程主函数
void* checkthmain(void* arg)
{
  while(true)
  {
    for(int ii=0; ii<vthid.size(); ii++)
    {
       //超时时间最好要大于处理业务的时间
       //如果已超时
       if((time(0)-vthid[ii].atime) > 25)
       {
          logfile.Write("thread %d(%lu) timeout(%d).\n",ii,vthid[ii].pthid,time(0)-vthid[ii].atime);

          //取消工作线程
          pthread_cancel(vthid[ii].pthid);

          //重新创建工作线程
          if(pthread_create(&vthid[ii].pthid,NULL,thmain,(void*)(long)ii) != 0)
          {
             logfile.Write("pthread_create() failed.\n");  EXIT(-1);
          }

          vthid[ii].atime=time(0);
       }
    }
    sleep(3);
  }
}

// 线程清理函数。
void thcleanup(void *arg) 
{
  pthread_mutex_unlock(&mutex);

  // 把本线程id从存放线程id的容器中删除。
  pthread_mutex_lock(&vthidlock);
  for (int ii=0;ii<vthid.size();ii++)
  {
    if (pthread_equal(pthread_self(),vthid[ii].pthid)) { vthid.erase(vthid.begin()+ii); break; }
  }
  pthread_mutex_unlock(&vthidlock);

  logfile.Write("线程%d(%lu)退出。\n",(int)(long)arg,pthread_self());
}

//业务处理线程主函数
void* thmain(void *arg)     // 线程主函数。
{
  int pthnum = (int)(long)arg;
  pthread_cleanup_push(thcleanup,arg);       // 把线程清理函数入栈。

  struct st_msg workmsg;

  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL);   // 线程取消方式为立即取消。

  pthread_detach(pthread_self());           // 把线程分离出去。

  while (true)
  {
    pthread_mutex_lock(&mutex);  // 给缓存队列加锁。

    // 如果缓存队列为空，等待，用while防止条件变量虚假唤醒。
    while (msgqueue.size()==0)
    {
      struct timeval now;
      gettimeofday(&now,NULL);
      now.tv_sec += 20;  //取二十秒之后的时间
      //这里cond只等待20秒，20之后还没被唤醒的话，自动醒来，更新线程活动时间，然后继续循环
      pthread_cond_timedwait(&cond,&mutex,(struct timespec*)&now);
      vthid[pthnum].atime=time(0); //更新当前线程的活动时间
    }

    //从缓存队列中获得一条信息
    workmsg = msgqueue.back();
    msgqueue.pop_back();
    pthread_mutex_unlock(&mutex);  // 给缓存队列解锁。


    //以下是业务处理代码
    char buffer_utf8[4096];
    //把GBK编码转换成UTF8编码
    GBKToUTF8(workmsg.buffer,buffer_utf8,sizeof(buffer_utf8));

    char actcode[10] = {};

    GetXMLBuffer(buffer_utf8,"actcode",actcode,9);
    
    char sendbuffer_utf8[10240] = {};
    char sendbuffer_gbk[10240] = {};

    switch(actcode[0])
    {
      ////////////////////////////////////////////////////////////////////////
      //以下为管理员模块
      //管理员登录
      case 'A':
	{
	  //管理员登录账号密码我直接固定下来，因为他权限太大，非必要不做更改
	  int adminid ; char adminpasswd[20]={};
	  GetXMLBuffer(buffer_utf8,"adminid",&adminid);
	  GetXMLBuffer(buffer_utf8,"adminpasswd",adminpasswd,19);
	  //登录失败
	  if(( adminid != 420729767 )  || (strcmp(adminpasswd,"twidc") != 0))
	  {
	    sprintf(sendbuffer_utf8,"<result>管理员账号或密码不正确</result>");
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    break;
	  }
	  //登录成功
	  else
	  {
	    pthread_mutex_lock(&mutex_sock_type);       
	    auto it = socktype.find(workmsg.sock);	  
	    (it->second).type = 1;
	    (it->second).id = adminid;
	    pthread_mutex_unlock(&mutex_sock_type);       
	    sprintf(sendbuffer_utf8,"<result>A</result>");
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    break;
	  }
	}
      //注册学生就餐卡
      case 'B':
	{
	  char stuname[30] = {}; char cardid[9] = { };
	  GetXMLBuffer(buffer_utf8,"stuname",stuname,29);

	  //接下来将就餐卡信息填入数据库
	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  sqlstatement stmt2(conn);
	  stmt1.prepare("insert into T_CARD values(SEQ_T_CARD.nextval,:1,100,1,sysdate)");
	  stmt1.bindin(1,stuname,sizeof(stuname));
	  stmt2.prepare("select SEQ_T_CARD.currval from dual");
	  stmt2.bindout(1,cardid,8);

	  pthread_mutex_lock(&T_CARD);
	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  else
	  {
	    if(stmt2.execute() != 0)
	    {
	      logfile.Write("execute sql failed, %s\n",stmt2.m_cda.message);
	      snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt2.m_cda.message);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	    stmt2.next();
	  }
	  pthread_mutex_unlock(&T_CARD);
	  conn->commit();
	  orapool.free(conn);
	  sprintf(sendbuffer_utf8,"<result>B</result><cardid>%s</cardid>",cardid);
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
      //注册店铺
      case 'C':
	{
	  char shopid[5] = {};
	  char shopname[31] = {}; char shoppasswd[31] = { };
	  GetXMLBuffer(buffer_utf8,"shopname",shopname,30);
	  GetXMLBuffer(buffer_utf8,"shoppasswd",shoppasswd,30);

	  //接下来将就餐卡信息填入数据库
	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  sqlstatement stmt2(conn);
	  stmt1.prepare("insert into T_SHOP values(SEQ_T_SHOP.nextval,:1,:2,1,sysdate)");
	  stmt1.bindin(1,shoppasswd,sizeof(shoppasswd));
	  stmt1.bindin(2,shopname,sizeof(shopname));
	  stmt2.prepare("select SEQ_T_SHOP.currval from dual");
	  stmt2.bindout(1,shopid,4);

	  pthread_mutex_lock(&T_SHOP);
	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  else
	  {
	    if(stmt2.execute() != 0)
	    {
	      logfile.Write("execute sql failed, %s\n",stmt2.m_cda.message);
	      snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt2.m_cda.message);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	    stmt2.next();
	  }
	  pthread_mutex_unlock(&T_SHOP);
	  conn->commit();
	  orapool.free(conn);
	  sprintf(sendbuffer_utf8,"<result>C</result><shopid>%s</shopid>",shopid);
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
      //查询被冻结的就餐卡
      case 'D':
	{
          char cardid[9] = {}; char stuname[31] = {};  	  
	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("select cardid,stuname from T_CARD where cardstatus = 0");
	  stmt1.bindout(1,cardid,8);
	  stmt1.bindout(2,stuname,30);
	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  //先发个回复
	  sprintf(sendbuffer_utf8,"<result>D</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);

	  while(true)
	  {
	    if(stmt1.next() != 0)
	    { break; }
            
	    sprintf(sendbuffer_utf8,"<cardid>%s</cardid><stuname>%s</stuname>",cardid,stuname);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	  }

	  orapool.free(conn);
	  break;
	}
      //就餐卡的冻结/解冻
      case 'E':
	{
	  char cardid[9] = {}; char cardstatus[4]={};
	  GetXMLBuffer(buffer_utf8,"cardid",cardid,8);	   
	  GetXMLBuffer(buffer_utf8,"cardstatus",cardstatus,3);	   

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("update T_CARD set cardstatus = :1 where cardid = :2");
	  stmt1.bindin(1,cardstatus,3);
	  stmt1.bindin(2,cardid,8);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  conn->commit();
	  orapool.free(conn);
	  sprintf(sendbuffer_utf8,"<result>E</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
	//查询已失效的店铺
      case 'F':
	{
	  char shopid[5] = {}; char shopname[31] = {};  	  
	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("select shopid,shopname from T_SHOP where shopstatus = 0");
	  stmt1.bindout(1,shopid,4);
	  stmt1.bindout(2,shopname,30);
	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  //先发个回复
	  sprintf(sendbuffer_utf8,"<result>F</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);

	  while(true)
	  {
	    if(stmt1.next() != 0)
	    { break; }

	    sprintf(sendbuffer_utf8,"<shopid>%s</shopid><shopname>%s</shopname>",shopid,shopname);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	  }

	  orapool.free(conn);
	  break;
	}
	//店铺的失效/恢复
      case 'G':
	{
	  char shopid[9] = {}; char shopstatus[4]={};
	  GetXMLBuffer(buffer_utf8,"shopid",shopid,8);	   
	  GetXMLBuffer(buffer_utf8,"shopstatus",shopstatus,3);	   

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("update T_SHOP set shopstatus = :1 where shopid = :2");
	  stmt1.bindin(1,shopstatus,3);
	  stmt1.bindin(2,shopid,8);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  conn->commit();
	  orapool.free(conn);
	  sprintf(sendbuffer_utf8,"<result>G</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
        //////////////////////////////////////////////////////////
	//以下为店铺模块
	//店铺的登录
       case 'H':
	{
	  char shopid[5] = {}; char shoppasswd[31] = {}; char shopname[31] = {}; char shopstatus[5] = {};
          GetXMLBuffer(buffer_utf8,"shopid",shopid,4);
          GetXMLBuffer(buffer_utf8,"shoppasswd",shoppasswd,30);

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("select shopname,shopstatus from T_SHOP where shopid = :1 and shoppasswd = :2");
	  stmt1.bindin(1,shopid,4);
	  stmt1.bindin(2,shoppasswd,30);
	  stmt1.bindout(1,shopname,30);
	  stmt1.bindout(2,shopstatus,4);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
        
	  //获取结果集
	  stmt1.next();

	  //得到一行结果,说明id和密码正确
	  if(stmt1.m_cda.rpc == 1) 
	  { 
	    //未失效,登录成功
	    if(strcmp(shopstatus,"1") == 0)
	    {
	      //信息录入shopid_sock中
	      pthread_mutex_lock(&mutex_shopid_sock);  
	      if( shopid_sock.find(atoi(shopid)) != shopid_sock.end())
	      {
		pthread_mutex_unlock(&mutex_shopid_sock);
		sprintf(sendbuffer_utf8,"<result>重复登陆!</result><shopname>%s</shopname>",shopname);
		UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
		Write(workmsg.sock,sendbuffer_gbk);
		orapool.free(conn);
		break;
	      }
	      shopid_sock[atoi(shopid)] = workmsg.sock;
	      pthread_mutex_unlock(&mutex_shopid_sock);  

	      //将socktype里的信息填写完整
	      pthread_mutex_lock(&mutex_sock_type);       
	      auto it = socktype.find(workmsg.sock);	  
	      (it->second).type = 2;
	      (it->second).id = atoi(shopid);
	      pthread_mutex_unlock(&mutex_sock_type);    

	      sprintf(sendbuffer_utf8,"<result>H</result><shopname>%s</shopname>",shopname);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	    else
	    {
	      sprintf(sendbuffer_utf8,"<result>当前店铺已被冻结，请尽快联系管理员</result><shopname>%s</shopname>",shopname);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	  }
	  //id或密码不正确
	  else
	  {
	    sprintf(sendbuffer_utf8,"<result>账号或密码不正确</result>",shopname);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	}
       //新增菜品	
       case 'I':
	{
	   char foodname[51] = {}; char costs[10] = {}; char shopid[5] ={}; char shopname[31] = {};
	   GetXMLBuffer(buffer_utf8,"foodname",foodname,50);
	   GetXMLBuffer(buffer_utf8,"costs",costs,9);
	   GetXMLBuffer(buffer_utf8,"shopid",shopid,4);
	   GetXMLBuffer(buffer_utf8,"shopname",shopname,30);
           
	   connection *conn = orapool.get();
	   sqlstatement stmt1(conn);
           stmt1.prepare("insert into T_FOOD values(SEQ_T_FOOD.nextval,:1,:2,0,:3,:4,sysdate)");
	   stmt1.bindin(1,foodname,50);
	   stmt1.bindin(2,costs,9);
	   stmt1.bindin(3,shopid,4);
	   stmt1.bindin(4,shopname,30);

	   if(stmt1.execute() != 0)
	   {
	     logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	     snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	     UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	     Write(workmsg.sock,sendbuffer_gbk);
	     orapool.free(conn);
	     break;
	   }
	   
	   //提交事务
	   conn->commit();
	   orapool.free(conn);

	   snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>I</result>",stmt1.m_cda.message);
	   UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	   Write(workmsg.sock,sendbuffer_gbk);
	   break;
	}
	//查询已有的菜品
       case 'J':
	{
           char shopid[5] = {}; 
	   char foodid[9] = {};	 char foodname[51] = {}; 
	   char costs[10] = {};  char consumptions[10] = {};
	   GetXMLBuffer(buffer_utf8,"shopid",shopid,4);

	   connection *conn = orapool.get();
	   sqlstatement stmt1(conn);
	   stmt1.prepare("select foodid,foodname,costs,consumptions from T_FOOD where shopid = :1 order by consumptions desc");
	   stmt1.bindin(1,shopid,4);
	   stmt1.bindout(1,foodid,8);
	   stmt1.bindout(2,foodname,50);
	   stmt1.bindout(3,costs,9);
	   stmt1.bindout(4,consumptions,9);

	   if(stmt1.execute() != 0)
	   {
	     logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	     snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	     UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	     Write(workmsg.sock,sendbuffer_gbk);
	     orapool.free(conn);
	     break;
	   }

	  //先发个回复
	  sprintf(sendbuffer_utf8,"<result>J</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);

	  while(true)
	  {
	    if (stmt1.next() !=0) { break; }
	    printf("%s %s %s %s\n",foodid,foodname,costs,consumptions);
            
	    sprintf(sendbuffer_utf8,"<foodid>%s</foodid><foodname>%s</foodname><costs>%s</costs><consumptions>%s</consumptions>",\
		                      foodid,foodname,costs,consumptions);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	  }

	  orapool.free(conn);
	  break;
	}
	//查询特定菜品的评论,这个先不写
       case 'K':
	{
          break; 
	}
	//改菜品名字或价格
       case 'L':
	{
	  char foodid[9] = {}; char foodname[51] = {}; char costs[10] = {};
	  GetXMLBuffer(buffer_utf8,"foodid",foodid,8);
	  GetXMLBuffer(buffer_utf8,"foodname",foodname,50);
	  GetXMLBuffer(buffer_utf8,"costs",costs,9);
	  
	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);

	  //改名和价格
	  if( (strlen(foodname) != 0) && (strlen(costs) != 0) )
	  {
	     stmt1.prepare("update T_FOOD set foodname = :1,costs = :2 where foodid = :3");
	     stmt1.bindin(1,foodname,50);
	     stmt1.bindin(2,costs,9);
	     stmt1.bindin(3,foodid,8);
	  }
	  //改名
	  if( (strlen(foodname) != 0) && (strlen(costs) == 0) )
          {
	     stmt1.prepare("update T_FOOD set foodname = :1 where foodid = :2");
	     stmt1.bindin(1,foodname,50);
	     stmt1.bindin(2,foodid,8);
	  }
	  //改价格
	  if( (strlen(foodname) == 0) && (strlen(costs) != 0) )
          {
	     stmt1.prepare("update T_FOOD set costs = :1 where foodid = :2");
	     stmt1.bindin(1,costs,9);
	     stmt1.bindin(2,foodid,8);
	  }

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  conn->commit();
	  orapool.free(conn);
	  //回复
	  sprintf(sendbuffer_utf8,"<result>L</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
       //删除菜品
       case 'M':
	{
	  char foodid[9] = {}; 
	  GetXMLBuffer(buffer_utf8,"foodid",foodid,8);	   

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("delete from T_FOOD where foodid = :1");
	  stmt1.bindin(1,foodid,8);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  conn->commit();
	  orapool.free(conn);
	  //回复
	  sprintf(sendbuffer_utf8,"<result>M</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
	//学生登录
       case 'N':
	{
	  char cardid[9] = {};  char stuname[31] = {}; char cardstatus[5] = {};
	  GetXMLBuffer(buffer_utf8,"cardid",cardid,8);

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("select stuname,cardstatus from T_CARD where cardid = :1 ");
	  stmt1.bindin(1,cardid,8);
	  stmt1.bindout(1,stuname,30);
	  stmt1.bindout(2,cardstatus,4);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  //获取结果集
	  stmt1.next();

	  //得到一行结果,说明id符合
	  if(stmt1.m_cda.rpc == 1) 
	  { 
	    //未失效,登录成功
	    if(strcmp(cardstatus,"1") == 0)
	    {
	      //将socktype里的信息填写完整
	      pthread_mutex_lock(&mutex_sock_type);       
	      auto it = socktype.find(workmsg.sock);	  
	      (it->second).type = 3;
	      (it->second).id = atoi(cardid);
	      pthread_mutex_unlock(&mutex_sock_type);    

	      sprintf(sendbuffer_utf8,"<result>N</result><stuname>%s</stuname>",stuname);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	    else
	    {
	      sprintf(sendbuffer_utf8,"<result>当前就餐卡已挂失，请尽快联系管理员</result><stuname>%s</stuname>",stuname);
	      UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	      Write(workmsg.sock,sendbuffer_gbk);
	      orapool.free(conn);
	      break;
	    }
	  }
	  //id不符合
	  else
	  {
	    sprintf(sendbuffer_utf8,"<result>不存在此id</result>",stuname);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	}
	//查询卡中余额
       case 'O':
	{
	  char cardid[9] = {}; char restmoney[10] = {};
	  GetXMLBuffer(buffer_utf8,"cardid",cardid,8);

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  stmt1.prepare("select restmoney from T_CARD where cardid = :1 ");
	  stmt1.bindin(1,cardid,8);
	  stmt1.bindout(1,restmoney,9);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  //获取结果集
	  stmt1.next();

	  //登录过后，其实卡号是一定对的,余额一定有。
	  if(stmt1.m_cda.rpc == 1) 
	  {
	    sprintf(sendbuffer_utf8,"<result>O</result><restmoney>%s</restmoney>",restmoney);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  else
	  {
	    sprintf(sendbuffer_utf8,"<result>不存在此id</result>",restmoney);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	}
	//充值卡余额
       case 'P':
	{
	  char cardid[9] = {}; char restmoney[10] = {}; char topupmoney[10] = {};
	  GetXMLBuffer(buffer_utf8,"cardid",cardid,8);
	  GetXMLBuffer(buffer_utf8,"topupmoney",topupmoney,9);

	  connection *conn = orapool.get();
	  sqlstatement stmt1(conn);
	  sqlstatement stmt2(conn);
	  stmt1.prepare("update T_CARD SET RESTMONEY = RESTMONEY + :1 where cardid = :2");
	  stmt1.bindin(1,topupmoney,9);
	  stmt1.bindin(2,cardid,8);
	  stmt2.prepare("select RESTMONEY from T_CARD where cardid = :1");
	  stmt2.bindin(1,cardid,8);
	  stmt2.bindout(1,restmoney,9);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  conn->commit();
	  
	  //上面一个stmt1执行成功，那么这个stmt2必然成功的，除非数据库崩溃了
	  stmt2.execute();
	  stmt2.next();

	  snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>P</result><restmoney>%s</restmoney>",restmoney);
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  orapool.free(conn);
	  break;
	}
	//查菜品，要寻找每一个上线的店铺的菜品
	//流程为,加锁 遍历shopid_sock里的shopid，然后记录下来，解锁， 然后根据每个记录查数据库
	//这个加锁解锁的地方是为了性能考虑，如果后续点菜的话，可能会出现商家已经下线的情况,出现这种情况我们会反馈给客端。
       case 'Q':
	{
	   char foodid[9] ={}; char foodname[51] = {}; char costs[10] = {}; char consumptions[10] = {};
	   char shopname[31] = {};
	   //得到所有上线的店铺的id
	   vector<int>  shopid ;
	   pthread_mutex_lock(&mutex_shopid_sock);
	   for(auto it = shopid_sock.begin() ; it != shopid_sock.end(); it++)
	   {
	      shopid.push_back(it->first);
	   }
	   pthread_mutex_unlock(&mutex_shopid_sock);

	   //先发送个回复
	   snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>Q</result>");
	   UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	   Write(workmsg.sock,sendbuffer_gbk);
	   //准备sql
           connection *conn = orapool.get();
           sqlstatement stmt1(conn);
	   stmt1.prepare("select foodid,foodname,shopname,costs,consumptions from T_FOOD where shopid = :1 order by consumptions desc");
	   stmt1.bindout(1,foodid,8);
	   stmt1.bindout(2,foodname,50);
	   stmt1.bindout(3,shopname,30);
	   stmt1.bindout(4,costs,9);
	   stmt1.bindout(5,consumptions,9);

	   //遍历上线的shopid
	   for( unsigned ii=0 ; ii<shopid.size(); ii++)
	   {
	     stmt1.bindin(1,&shopid[ii]);
	     if(stmt1.execute() != 0)
	     {
	       logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	       snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	       UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	       Write(workmsg.sock,sendbuffer_gbk);
	       orapool.free(conn);
	       break;
             }

	     while(true)
	     {
	       if(stmt1.next() != 0) {break;}

	       sprintf(sendbuffer_utf8,"<foodid>%s</foodid><foodname>%s</foodname><shopname>%s</shopname><costs>%s</costs><consumptions>%s</consumptions>",\
		                          foodid,foodname,shopname,costs,consumptions);
	       UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	       Write(workmsg.sock,sendbuffer_gbk);
	     }
	   }
	   orapool.free(conn);
	   break;
	}
	//查评论，先不写
       case 'R':
	{
	  break;
	}
	//按照金额购买商品
       case 'S':
	{
	  //收到的
	  char cardid[9] = {}; char foodid[9] = {}; 
	  //必须要返回的
	  char costs[10] = {}; char restmoney[10] = {};

          //业务流程需要的
	  char stuname[31] = {}; char shopid[5] ={}; char shopname[31] = {};
	  char foodname[51] = {}; 

	  GetXMLBuffer(buffer_utf8,"cardid",cardid,8);  
	  GetXMLBuffer(buffer_utf8,"foodid",foodid,8);  
          
	  connection *conn = orapool.get();

	  //查T_CARD里的stuname  restmoney
          sqlstatement stmt1(conn);
	  stmt1.prepare("select stuname,restmoney from T_CARD where cardid = :1");
	  stmt1.bindin(1,cardid,8);
	  stmt1.bindout(1,stuname,30);
	  stmt1.bindout(2,restmoney,9);
	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  stmt1.next();
	  //查T_FOOD里的foodname costs shopid shopname
	  sqlstatement stmt2(conn);
	  stmt2.prepare("select foodname,costs,shopid,shopname from T_FOOD where foodid = :1");
	  stmt2.bindin(1,foodid,8);
	  stmt2.bindout(1,foodname,50);
	  stmt2.bindout(2,costs,9);
	  stmt2.bindout(3,shopid,4);
	  stmt2.bindout(4,shopname,30);
	  if(stmt2.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt2.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt2.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  stmt2.next();
	  //修改就餐余额
	  double dcosts = atof(costs); double drestmoney = atof(restmoney);
	  if( drestmoney < dcosts ) //钱不够
	  {
	    sprintf(sendbuffer_utf8,"<result>余额不足</result>");
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
          sqlstatement stmt3(conn);
	  stmt3.prepare("update T_CARD set restmoney = restmoney - :1 where cardid = :2");
	  stmt3.bindin(1,costs,9);
	  stmt3.bindin(2,cardid,8);
	  if(stmt3.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt3.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt3.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }
	  //插入一条消费记录
	  sqlstatement stmt4(conn);
	  stmt4.prepare("insert into t_consumptions values(seq_t_consumptions.nextval,:1,:2,:3,:4,:5,:6,:7,sysdate)");
	  stmt4.bindin(1,cardid,8);
	  stmt4.bindin(2,stuname,30);
	  stmt4.bindin(3,shopid,4);
	  stmt4.bindin(4,shopname,30);
	  stmt4.bindin(5,foodid,8);
	  stmt4.bindin(6,foodname,50);
	  stmt4.bindin(7,costs,9);
	  if(stmt4.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt4.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt4.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    conn->rollback(); //事务回滚，就餐余额恢复。
	    orapool.free(conn);
	    break;
	  }
	  //修改销量
	  sqlstatement stmt5(conn);
	  stmt5.prepare("update T_FOOD set consumptions = consumptions + 1 where foodid = :1");
	  stmt5.bindin(1,foodid,4);
	  if(stmt5.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt5.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt5.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    conn->rollback(); //事务回滚，就餐余额与消费记录表恢复。
	    orapool.free(conn);
	    break;
	  }

	  //通知相应的商家和学生
	  int ishopid = atoi(shopid); bool isonline = true; int shopsock;
//map<int,int> shopid_sock;
//pthread_mutex_t mutex_shopid_sock = PTHREAD_MUTEX_INITIALIZER; //map的锁
           pthread_mutex_lock(&mutex_shopid_sock);
	   auto it = shopid_sock.find(ishopid);
	   //如果没找到说明在学生浏览和下单之间店铺下线了.
	   if(it == shopid_sock.end())
	   {
	      isonline = false;
	   }
	   else
	   {
	      shopsock = it->second;
	   }
	   pthread_mutex_unlock(&mutex_shopid_sock);

	   if(isonline)
	   {
	     //给店铺
	     sprintf(sendbuffer_utf8,"<result>Y</result><cardid>%s</cardid><foodid>%s</foodid><foodname>%s</foodname>",\
		 cardid,foodid,foodname);
	     UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	     Write(shopsock,sendbuffer_gbk);

	     //给学生
	     sprintf(sendbuffer_utf8,"<result>S</result><costs>%s</costs><restmoney>%.2lf</restmoney>",costs,drestmoney-dcosts);
	     UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	     Write(workmsg.sock,sendbuffer_gbk);
	     conn->commit(); //提交事务
	     orapool.free(conn);
	     break;
	   }
	   else
	   {
	     //给学生
	     sprintf(sendbuffer_utf8,"<result>店铺已经下线</result>");
	     UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	     Write(workmsg.sock,sendbuffer_gbk);
	     conn->rollback();
	     orapool.free(conn);
	     break;
	   }
	}
	//对已购买的菜进行评论,先不写了
       case 'T':
	{
	  break;
	}
	//查询本卡的消费记录
       case 'U':
	{
	   char cardid[9] = {}; 
	   GetXMLBuffer(buffer_utf8,"cardid",cardid,8);

           //需要返回的信息
	   char orderid[11] = {};
	   char stuname[31] = {}; char shopid[5] = {}; char shopname[31] = {};
	   char foodid[9] = {};  char foodname[51] = {}; char costs[10] = {};
	   char uptime[51] = {};

	   connection *conn = orapool.get();
	   sqlstatement stmt1(conn);
	   stmt1.prepare("select orderid,cardid,stuname,shopid,shopname,foodid,foodname,costs,to_char(uptime,'yyyy-mm-dd hh24:mi:ss') \
	       from T_CONSUMPTIONS where cardid = :1");
	   stmt1.bindin(1,cardid,8);
	   stmt1.bindout(1,orderid,10);
	   stmt1.bindout(2,cardid,8);
	   stmt1.bindout(3,stuname,30);
	   stmt1.bindout(4,shopid,4);
	   stmt1.bindout(5,shopname,30);
	   stmt1.bindout(6,foodid,8);
	   stmt1.bindout(7,foodname,50);
	   stmt1.bindout(8,costs,9);
	   stmt1.bindout(9,uptime,50);

	  if(stmt1.execute() != 0)
	  {
	    logfile.Write("execute sql failed, %s\n",stmt1.m_cda.message);
	    snprintf(sendbuffer_utf8,sizeof(sendbuffer_utf8),"<result>数据库错误 %s</result>",stmt1.m_cda.message);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	    orapool.free(conn);
	    break;
	  }

	  //先发个回复
	  sprintf(sendbuffer_utf8,"<result>U</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);

	  while(true)
	  {
	    if(stmt1.next() != 0) {break;}

	    sprintf(sendbuffer_utf8,"<orderid>%s</orderid><cardid>%s</cardid>"\
		"<stuname>%s</stuname><shopid>%s</shopid><shopname>%s</shopname>"\
		"<foodid>%s</foodid><foodname>%s</foodname><costs>%s</costs><uptime>%s</uptime>",\
		orderid,cardid,stuname,shopid,shopname,foodid,foodname,costs,uptime);
	    UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	    Write(workmsg.sock,sendbuffer_gbk);
	  }

	  orapool.free(conn);
	  break;
	}
	//心跳报文
	case 'Z':
	{
	  sprintf(sendbuffer_utf8,"<result>Z</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
          break;
	}

       default:
	{
	  sprintf(sendbuffer_utf8,"<result>actcode error</result>");
	  UTF8ToGBK(sendbuffer_utf8,sendbuffer_gbk,sizeof(sendbuffer_gbk));
	  Write(workmsg.sock,sendbuffer_gbk);
	  break;
	}
	
    }
    //业务处理完毕
    ////////////////////////////
    //回到主while循环
  }

  pthread_cleanup_pop(1);  //一定要写才能通过编译,理论上我们不会执行到这里来
}

