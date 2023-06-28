#ifndef QUIC_H_
#define QUIC_H_

#include <stdint.h>
#include "mutex_type.h" /* Needed for mutex_type */
#include <msquic.h>
#include "Socket.h"
#include "Clients.h"

typedef HQUIC QSOCKET;


void MSQUIC_initialize(void);

void QUIC_handleInit(int);

void QUIC_outInitialize(void);
void QUIC_outTerminate(void);

QSOCKET QUIC_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc);

int QUIC_getch(QUIC_CTX* q_ctx, char* c);
char *QUIC_getdata(QUIC_CTX* q_ctx, size_t bytes, size_t* actual_len, int* rc);
int QUIC_putdatas(QSOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs);
int QUIC_close(QSOCKET socket);

int QUIC_new(const char* addr, size_t addr_len, int port, networkHandles* net, long timeout);


int QUIC_noPendingWrites(QSOCKET socket);
char* QUIC_getpeer(QSOCKET sock);

void QUIC_addPendingWrite(QSOCKET socket);
void QUIC_clearPendingWrite(QSOCKET socket);

typedef void QUIC_writeContinue(QSOCKET socket);
void QUIC_setWriteContinueCallback(QUIC_writeContinue*);

typedef void QUIC_writeComplete(QSOCKET socket, int rc);
void QUIC_setWriteCompleteCallback(QUIC_writeComplete*);

typedef void QUIC_writeAvailable(QSOCKET socket);
void QUIC_setWriteAvailableCallback(QUIC_writeAvailable*);


// Callbacks
QUIC_STATUS QUIC_API ClientStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

QUIC_STATUS QUIC_API ClientConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);

#endif // QUIC_H_
