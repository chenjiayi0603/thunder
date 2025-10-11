DEV_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

PUBLIC_INC = $(DEV_DIR)public
NET_INC =  
TOOL_INC =  
BUS_INC = 
FRAME_BASE_INC = 
PROTO_INC =  
SSL_INC = /usr/local/openssl/include
BSON_INC = /root/protocol/libsignal-protocol-c-2.3.0/src
MONGODB_INC = /usr/local/include/mongocxx/v_noabi
REDIS_INC = /usr/local/include/signal
LIB_NAME =  -lprotobuf    -lpthread -ldl  -lssl -lcrypto    -lsignal-protocol-c

INC     = -I./ -I$(PUBLIC_INC) -I$(NET_INC) -I$(TOOL_INC) -I$(FRAME_BASE_INC) -I$(PROTO_INC) -I$(BUS_INC) -I$(SSL_INC)  -I/usr/include/openssl  -I$(BSON_INC) -I$(MONGODB_INC) -I$(REDIS_INC)

LIB     = -L$(DEV_DIR)comm/libnet -L$(DEV_DIR)comm/libtool -L$(DEV_DIR)comm/libbusiness $(LIB_NAME) -L/usr/lib -L/root/mysql-5.5.62-linux-glibc2.12-x86_64/lib -L/usr/local/lib -L/usr/local/openssl/lib -L/usr/local/my_lib  -L./

CC = /usr/bin/gcc
CXX = /usr/bin/g++

CFLAGS = -Wpointer-arith -O3 -Wall -pipe -D_REENTRANT -D_GNU_SOURCE -DOPEN_PRINT -fPIC  -g -std=c++11


BASE_SRC = $(DEV_DIR)server/base/src


HEADS = $(wildcard *.h)
HEADS += $(wildcard business/*.h)

SOURCES = $(wildcard *.cpp)
SOURCES += $(wildcard $(BASE_SRC)/*.cpp)
OURCES += $(wildcard *.cc)


OBJS = $(SOURCES:.cpp=.o)
TARGET = encrypt_demo_test

all:$(TARGET)
$(TARGET):$(OBJS) 
	$(CXX) $(OBJS) -g -o $(TARGET) $(LIB)

$(OBJS):%.o:%.cpp $(HEADS)
	$(CXX) $(CFLAGS) $(INC) -c $< -o $@

install:
	cp $(TARGET) $(BIN_DIR) -f

clean:
	rm -rf $(TARGET) $(OBJS)

