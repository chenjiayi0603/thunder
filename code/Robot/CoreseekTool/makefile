CC = gcc
CXX = g++
CFLAGS = -g -O0 #-Wall
CPPFLAGS =  -D_GNU_SOURCE=1 -D_REENTRANT -fPIC  
#CXXFLAG = -g -O2 -Wall -fdiagnostics-show-caret -Wextra -pedantic -Wstrict-aliasing -Wstrict-overflow -Woverloaded-virtual -Wold-style-cast -Wc++0x-compat -Wc++11-compat -Wundef -Wshadow -Wformat -Wsuggest-attribute=format -Wsuggest-attribute=noreturn -Wno-variadic-macros -fvisibility=hidden
CXXFLAG = -O0 -Wall -ggdb -m64 -Wl,--export-dynamic # -lrt -std=c++11 -pthread  #-static-libgcc -static-libstdc++

DEFINE_SERVER = 


ifeq ($(unit_test),y)
CPPFLAGS += -DUNIT_TEST
endif

ARCH:=$(shell uname -m)

ARCH32:=i686
ARCH64:=x86_64

ifeq ($(ARCH),$(ARCH64))
SYSTEM_LIB_PATH:=/usr/lib64
else
SYSTEM_LIB_PATH:=/usr/lib
endif
LIB3RD_PATH = ../../l3oss
LOSS_PATH = ../../loss

#输出目录在 RobotServer执行目录
BIN_PATH = ../../../deploy/RobotServer/bin

VPATH = . #$(STARSHIP_PATH)/src
SUB_DIRS := $(foreach dir, $(VPATH), $(shell find $(dir) -maxdepth 5 -type d))
DIRS := $(SUB_DIRS)


INC := $(INC) \
       -I $(LIB3RD_PATH)/include \
       -I $(LIB3RD_PATH)/include/mysql \
       -I $(LOSS_PATH)/src \

#loss库需要protobuf和ev和jemalloc
LDFLAGS := $(LDFLAGS) -D_LINUX_OS_ \
           -L$(LIB3RD_PATH)/lib -lmysqlclient_r \
           -L$(LIB3RD_PATH)/lib -llog4cplus \
           -L$(LIB3RD_PATH)/lib -lprotobuf \
           -L$(LIB3RD_PATH)/lib -lev \
           -L$(LIB3RD_PATH)/lib -ljemalloc \
           -L$(SYSTEM_LIB_PATH) -lpthread \
           -L$(SYSTEM_LIB_PATH) -lz -ldl -lrt \
           -L$(LOSS_PATH)/lib -lloss 

SUB_INCLUDE = 
COMM_PATH = . \
            ..

SO_DEP_PATH = ..

COMM_CPP_SRCS = $(foreach dir, $(COMM_PATH), $(wildcard $(dir)/*.cpp))
COMM_CC_SRCS = $(foreach dir, $(COMM_PATH), $(wildcard $(dir)/*.cc))
COMM_C_SRCS = $(foreach dir, $(COMM_PATH), $(wildcard $(dir)/*.c))
COMM_OBJS = $(patsubst %.cpp,%.o,$(COMM_CPP_SRCS)) $(patsubst %.c,%.o,$(COMM_C_SRCS)) $(patsubst %.cc,%.o,$(COMM_CC_SRCS)) 

CPP_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cpp))
CC_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cc))
C_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.c))

CMD_DEP_CPP_SRCS = $(foreach dir, $(SO_DEP_PATH), $(wildcard $(dir)/*.cpp))

CPP_SRCS_OBJS = $(patsubst %.cpp,%.o,$(CPP_SRCS))
CC_SRCS_OBJS = $(patsubst %.cpp,%.o,$(CC_SRCS))
C_SRCS_OBJS = $(patsubst %.c,%.o,$(C_SRCS))
CMD_DEP_CPP_SRCS_OBJS = $(patsubst %.cpp,%.o,$(CMD_DEP_CPP_SRCS))

OBJS = $(CPP_SRCS_OBJS) $(CC_SRCS_OBJS) $(C_SRCS_OBJS) $(CMD_DEP_CPP_SRCS_OBJS) 


TARGET = CoreseekTool

all: $(TARGET)

CoreseekTool: $(OBJS)
	$(CXX) $(CXXFLAG) $(DEFINE_SERVER) $(UNITTEST) -o $@ $(OBJS) $(LDFLAGS) $(INC)
	cp $(TARGET) $(BIN_PATH)
	
%.o:%.cpp
	$(CXX) $(INC) $(CPPFLAGS) $(CFLAGS) $(DEFINE_SERVER) $(UNITTEST) -c -o $@ $< $(LDFLAGS)
%.o:%.cc
	$(CXX) $(INC) $(CPPFLAGS) $(UNITTEST) -c -o $@ $< $(LDFLAGS)
%.o:%.c
	$(CC) $(INC) $(CFLAGS) $(UNITTEST) -c -o $@ $< $(LDFLAGS)
clean:
	rm -f $(CPP_SRCS_OBJS) $(CC_SRCS_OBJS) $(C_SRCS_OBJS) $(CMD_DEP_CPP_SRCS_OBJS)
	rm -f $(TARGET)
        
        
