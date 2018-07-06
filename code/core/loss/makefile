CXX = $(shell command -v ccache >/dev/null 2>&1 && echo "ccache g++" || echo "g++")
CXXFLAG = -Wall -Wno-format -std=c++11  -g -O2 -fPIC -fno-strict-aliasing -Wno-sign-compare

ARCH:=$(shell uname -m)

ARCH32:=i686
ARCH64:=x86_64

ifeq ($(ARCH),$(ARCH64))
SYSTEM_LIB_PATH:=/usr/lib64
else
SYSTEM_LIB_PATH:=/usr/lib
endif

LIB3RD_PATH = ../l3oss
LOSS_PATH = .
PROTO_PATH = ../proto
DEPLOY_LIB = ../../../deploy/lib/

#暂时把框架节点协议放入loss
VPATH = $(LOSS_PATH)/src $(PROTO_PATH)/src
SUB_DIRS := $(foreach dir, $(VPATH), $(shell find $(dir) -maxdepth 5 -type d))
DIRS := $(SUB_DIRS) 

INC := $(INC) \
       -I /usr/include \
       -I $(LIB3RD_PATH)/include \
       -I $(LIB3RD_PATH)/include/mariadb \
       -I $(LOSS_PATH)/src

LDFLAGS := $(LDFLAGS) -D_LINUX_OS_ \
           -L$(LIB3RD_PATH)/lib -lprotobuf \
           -L$(LIB3RD_PATH)/lib -llog4cplus \
           -L$(LIB3RD_PATH)/lib -lev \
           -L$(SYSTEM_LIB_PATH) -lpthread \
           -L$(SYSTEM_LIB_PATH) -lc -ldl -lrt

CPP_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cpp))
CC_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cc))
C_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.c))
OBJS = $(patsubst %.cpp,%.o,$(CPP_SRCS)) $(patsubst %.c,%.o,$(C_SRCS)) $(patsubst %.cc,%.o,$(CC_SRCS))

LIBRARYS: $(OBJS)
	ar -crv libloss.a $(OBJS)
	ranlib libloss.a
	$(CXX) -shared -Wl,-soname,libloss.so -o libloss.so $(OBJS) -lc -lrt -ldl $(LDFLAGS)
	test -d ${DEPLOY_LIB} && install libloss.so ${DEPLOY_LIB}
	install -v libloss.a $(LOSS_PATH)/lib/
	install -v libloss.so $(LOSS_PATH)/lib/

%.o:%.cpp
	$(CXX) $(INC) $(CXXFLAG) -c -o $@ $< 
%.o:%.cc
	$(CXX) $(INC) $(CXXFLAG) -c -o $@ $< 
%.o:%.c
	$(CXX) $(INC) $(CXXFLAG) -c -o $@ $< 
clean:
	rm -f $(OBJS)
	rm -f $(LIBRARYS) 

cleanall:
	rm -f $(OBJS)
	rm -f $(LIBRARYS) 
	rm libloss.a libloss.so ./lib/libloss.a ./lib/libloss.so
        
        
