/*******************************************************************************
 * Copyright (c) 2023 EMQ Technologies Co., William Yang and others.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
*/

#ifndef QUIC_H_
#define QUIC_H_

#include <stdint.h>
#include "mutex_type.h" /* Needed for mutex_type */
#include <msquic.h>
#include "Socket.h"
#include "Clients.h"

typedef HQUIC QSOCKET;

enum ZERO_RTT {
    ZERO_RTT_DISABLED = 0,
    ZERO_RTT_ENABLED,
    ZERO_RTT_AUTO,
};

#define QUIC_SOCKET_SUCCESS TCPSOCKET_COMPLETE;
#define QUIC_SOCKET_ERROR SOCKET_ERROR;
#define QUIC_SOCKET_INTERRUPTED TCPSOCKET_INTERRUPTED;
void MSQUIC_initialize(void);

void QUIC_handleInit(int);

void QUIC_outInitialize(void);
void QUIC_outTerminate(void);

SOCKET QUIC_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc);

int QUIC_getch(QUIC_CTX* q_ctx, char* c);
char *QUIC_getdata(QUIC_CTX* q_ctx, size_t bytes, size_t* actual_len, int* rc);
int QUIC_putdatas(QUIC_CTX* q_ctx, char* buf0, size_t buf0len, PacketBuffers bufs);
int QUIC_close(networkHandles *net, QUIC_UINT62 reason);

int QUIC_new(const char* addr, size_t addr_len, int port, networkHandles* net, MQTTClient_SSLOptions *sslopts, long timeout);
int QUIC_start_0RTT_connection(QUIC_CTX* q_ctx);

int QUIC_noPendingWrites(QSOCKET socket);
char* QUIC_getpeer(QUIC_CTX* q_ctx);

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
