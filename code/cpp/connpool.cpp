#include "connpool.h"

//数据库连接池类的定义
connpool::connpool()
{
   m_maxconns=0;
   m_timeout=0;
   memset(m_connstr,0,sizeof(m_connstr));
   memset(m_charset,0,sizeof(m_charset));
   m_conn=0;
}

connpool::~connpool()
{
  destroy();
}

//初始化数据库连接池，初始化锁
bool connpool::init(char* connstr,char* charset,const int maxconns, const int timeout)
{
  //检查数据库连接参数是否可用
  connection conn;
  if(conn.connecttodb(connstr,charset) != 0)
  {
     printf("数据库连接失败.\n%s\n",conn.m_cda.message);
     return false;
  }
  conn.disconnect();

  m_maxconns=maxconns;
  m_timeout=timeout;
  strncpy(m_connstr,connstr,100);
  strncpy(m_charset,charset,100);

  m_conn = new struct st_conn[m_maxconns];

  for(int ii=0; ii<m_maxconns; ii++)
  {
    pthread_mutex_init(&m_conn[ii].mutex,0);
    m_conn[ii].atime=0;
  }

  return true;
}

//断开数据库连接，销毁锁，释放内存
void connpool::destroy()
{
  for(int ii=0; ii<m_maxconns; ii++)
  {
    m_conn[ii].conn.disconnect();  //断开数据库连接
    pthread_mutex_destroy(&m_conn[ii].mutex); //销毁锁
  }
  delete[] m_conn;
  m_conn=0;

  m_maxconns=0;
  m_timeout=0;
  memset(m_connstr,0,sizeof(m_connstr));
  memset(m_charset,0,sizeof(m_charset));
}

//从数据库连接池中获得一个空闲的连接，返回连接的地址
connection* connpool::get()
{
  int pos=-1; //用于记录第一个未连接数据库的数组地址

  for(int ii=0; ii<m_maxconns; ii++)
  {
    if(pthread_mutex_trylock(&m_conn[ii].mutex)==0) //尝试加锁 
    {
       if(m_conn[ii].atime>0)  //如果是连接好的状态
       {
          //如果之前有加锁的未连接好的位置,那么释放锁
          if(pos != -1) { pthread_mutex_unlock(&m_conn[pos].mutex); }
          m_conn[ii].atime=time(0);
          printf("取到%d.\n",ii);
          return &m_conn[ii].conn;  //返回数据库连接的地址
       }

         //如果是未连接好的状态,且之前还没有发现其他未连接位置,那么先记录下位置，并且这里不解开锁
         if(pos==-1) {pos=ii;}
         else { pthread_mutex_unlock(&m_conn[ii].mutex);} //如果之前就发现了未连接的位置，这里的锁解开

    }
  }

  if(pos==-1)  //连接池已用完
  {
    printf("连接池已用完.\n"); return NULL;
  }

  //数据库连接池没有用完
  if(m_conn[pos].conn.connecttodb(m_connstr,m_charset) !=0)
  {
    pthread_mutex_unlock(&m_conn[pos].mutex);  //释放锁
    return NULL;
  }

  //把数据库连接的使用时间设置为当前时间
  //printf("新连接%d\n",pos);
  m_conn[pos].atime=time(0);
  return &m_conn[pos].conn;
}

//归还数据库连接
bool connpool::free(connection* conn)
{
  for(int ii=0; ii<m_maxconns; ii++)
  {
    if(&m_conn[ii].conn==conn)
    {
       m_conn[ii].atime=time(0);  //把数据库的时间设置为当前时间
       pthread_mutex_unlock(&m_conn[ii].mutex);
       //printf("归还%d.\n",ii);
       return true;
    }
  }

  return false;
}

//检查数据库连接池，断开空闲的连接,在服务程序中用一个专用的子线程调用此函数
void connpool::checkpool()
{
   
  for(int ii=0; ii<m_maxconns; ii++)
  { 
    if(pthread_mutex_trylock(&m_conn[ii].mutex)==0) //尝试加锁 
    {  
       if(m_conn[ii].atime>0)  //如果是连接好的状态
       {  
          //判断连接是否超时
          if( (time(0) - m_conn[ii].atime) > m_timeout)
          {  
             m_conn[ii].conn.disconnect(); //断开数据库连接
             m_conn[ii].atime=0;           //重置数据库连接使用时间
             printf("超时断开 %d.\n",ii);
          }
          else
          {  
             //如果没有超时，执行一次sql,验证连接是否有效，如果无效，断开它
             //如果网络断开，或者数据库重启，那么就需要从重新连接数据库，在这里只需要断开连接就行了
             //重连的操作交给get()函数
             if( m_conn[ii].conn.execute("select * from dual") != 0)
             {  
                m_conn[ii].conn.disconnect();   //断开连接
                m_conn[ii].atime=0;           //重置数据库连接使用时间
             }
          }
       }
       pthread_mutex_unlock(&m_conn[ii].mutex); //释放锁 
    }
    //加锁失败表示数据库连接正在使用中，不需要检查
  }
}

//////////////////数据库连接池类定义结束//////////////////////////////////
//////////////////////////////////////////////////////////////////////////



