CC = gcc
CXX = g++
# 与 code/3party/protobuf 构建一致：protobuf 3.5+/7.x 生成代码需 C++17/20 与对应头路径
CXXFLAG = -g -Og -fPIC -DNODE_BEAT=10.0 -Wall -Wno-deprecated-declarations -Wno-format -std=c++20 -Wstrict-overflow

ARCH:=$(shell uname -m)

ARCH32:=i686
ARCH64:=x86_64

ifeq ($(ARCH),$(ARCH64))
SYSTEM_LIB_PATH:=/usr/lib64
else
SYSTEM_LIB_PATH:=/usr/lib
endif
3PARTY_PATH = ../../3party
PROTO_PATH = .
NET_PATH = ../../Net

VPATH = .

SUB_DIRS := $(VPATH)
#SUB_DIRS := $(foreach dir, $(VPATH), $(shell find $(dir) -maxdepth 5 -type d))
DIRS := $(SUB_DIRS)


# protobuf 头必须优先于 code/3party/include（避免旧版 google/protobuf 与构建目录不一致）
INC := $(INC) \
       -I $(PROTO_PATH) \
       -I $(PROTO_PATH)/src   \
       -I $(NET_PATH)/src \
       -I $(3PARTY_PATH)/protobuf/src \
       -I $(3PARTY_PATH)/protobuf/build/_deps/absl-src \
       -I $(3PARTY_PATH)/include

# 与 libprotobuf.so 一致：链 utf8_validity（libprotobuf 的 NEEDED）
LDFLAGS := $(LDFLAGS) -D_LINUX_OS_ \
           -L$(3PARTY_PATH)/protobuf/build \
           -L$(3PARTY_PATH)/protobuf/build/third_party/utf8_range \
           -lprotobuf -lutf8_validity \
           -L$(3PARTY_PATH)/lib \
           -L$(SYSTEM_LIB_PATH) -lpthread \
           -L$(SYSTEM_LIB_PATH) -lc -ldl -lrt

CPP_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cpp))
CC_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.cc))
C_SRCS = $(foreach dir, $(DIRS), $(wildcard $(dir)/*.c))
OBJS = $(patsubst %.cpp,%.o,$(CPP_SRCS)) $(patsubst %.c,%.o,$(C_SRCS)) $(patsubst %.cc,%.o,$(CC_SRCS))

SO_PATH = . 
PB_OBJS = $(patsubst %.pb.cc,%.pb.o,$(CC_SRCS))

TARGET = libProto.so

all: $(TARGET)

$(TARGET): $(OBJS) 
	$(CXX) -fPIC -rdynamic -shared -g -o $@ $^ $(LDFLAGS)
	cp $(TARGET) ../../../deploy/lib

%.o:%.cpp
	$(CXX) $(INC) $(CXXFLAG) -c -o $@ $< $(LDFLAGS)
%.o:%.cc
	$(CXX) $(INC) $(CXXFLAG) -c -o $@ $< $(LDFLAGS)
%.o:%.c
	$(CC) $(INC) $(CXXFLAG) -c -o $@ $< $(LDFLAGS)
clean:
	rm -f $(OBJS) 
	rm -f $(TARGET)
        
        
