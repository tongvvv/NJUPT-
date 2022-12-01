#开发框架头文件路径
PUBINCL = -I/canteen/public

#开发框架cpp文件名，直接包含进来
PUBCPP = /canteen/public/_public.cpp

#oracle头文件路径
ORAINCL = -I$(ORACLE_HOME)/rdbms/public -I/canteen/public

#oracle库文件路径
ORALIB =  -L$(ORACLE_HOME)/lib -L.

#oracle的oci库
ORALIBS = -lclntsh

#开发框架oracle的cpp文件名，直接包含进来
ORACPP = /canteen/public/_ooci.cpp

#编译参数
CFLAGS = -g

all:procctl checkproc mainserver 

procctl:procctl.cpp
		g++ $(CFLAGS) -o procctl procctl.cpp -lpthread
		cp procctl ../bin/.

checkproc:checkproc.cpp
		g++ $(CFLAGS) -o checkproc checkproc.cpp $(PUBINCL) $(PUBCPP) -lm -lc -lpthread
		cp checkproc ../bin/.

mainserver:mainserver.cpp connpool.h connpool.cpp
		g++ $(CFLAGS) -o mainserver mainserver.cpp connpool.cpp $(PUBINCL) $(PUBCPP) $(ORAINCL) $(ORALIB) $(ORALIBS) $(ORACPP) -lpthread --std=c++11
		cp mainserver ../bin/.
