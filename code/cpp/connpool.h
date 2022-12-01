#include "_ooci.h"
#include "_public.h"

//数据库连接池类
class connpool
{
private:
  struct st_conn
  {
    connection conn ;       //数据库连接
    pthread_mutex_t mutex;  //用于数据库连接的互斥锁
    time_t atime;           //上次连接的时间,如果未连接则取0
  } *m_conn;   //数据库连接池

  int  m_maxconns;      //最大连接数
  int  m_timeout;       //数据库连接超时时间，单位:秒
  char m_connstr[101]; //数据库连接的参数
  char m_charset[101]; //数据库的字符集
public:
  connpool();
 ~connpool();

  //初始化数据库连接池，初始化锁
  bool init( char* connstr,char* charset,const int maxconns, const int timeout);
  //断开数据库连接，销毁锁，释放内存
  void destroy();

  //从数据库连接池中获得一个空闲的连接，返回连接的地址
  //如果连接池已经用完，或连接数据库失败，返回空
  connection* get();
  //归还数据库连接
  bool free(connection* conn);

  //检查数据库连接池，断开空闲的连接,在服务程序中用一个专用的子线程调用此函数
  void checkpool();
};
