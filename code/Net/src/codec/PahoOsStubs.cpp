// OS-level stubs for paho-mqtt-c symbols needed by CodecMqtt.
// paho utility .c files are compiled directly into Net/Hello/Center targets.
// Only OS-specific symbols remain.
#include <cstdio>
#include <cstdlib>

extern "C" {

void Paho_thread_lock_mutex(void* m) {}
void Paho_thread_unlock_mutex(void* m) {}
int  Paho_thread_getid() { return 0; }

int  WebSocket_putdatas(void* ws, char* buf, size_t len, char* header) { return 0; }
int  WebSocket_getch(void* ws, char* c) { return -1; }
int  WebSocket_getdata(void* ws, char* buf, size_t len) { return -1; }
int  WebSocket_framePos(void* ws, char* buf, int len, int* pos) { return -1; }
void WebSocket_framePosSeekTo(void* ws, int pos) {}
void WebSocket_upgrade(void* ws) {}
int  WebSocket_connect(void* ws, const char* url) { return 0; }

void* MQTTProtocol_createMessage(void* publish, int mm, int qos) { return 0; }
void  MQTTProtocol_closeSession(int sock, int wait) {}
int   Protocol_processPublication(void* p, int sock) { return 0; }

int Socket_getpeer(int sock, char* name, int len) { return 0; }
void* Socket_new(const char* host, int port, const char* bind) { return 0; }
int Socket_noPendingWrites(int sock) { return 0; }
void SocketBuffer_updateWrite(int sock, int bytes) {}
void Socket_close(int sock) {}
int  Proxy_connect(void* sock, int ssl, const char* url, const char* origin) { return 0; }

int bstate = 0;
void* state = 0;
int (*clientSocketCompare)(int, int) = 0;
int (*Proxy_noProxy)(const char*) = 0;
int (*Proxy_setHTTPProxy)(const char*) = 0;

} // extern "C"
