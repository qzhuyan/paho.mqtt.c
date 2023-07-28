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
#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include "msquic.h"
#include "Socket.h"


#define PEER_LEN INET6_ADDRSTRLEN+7

enum shutdown_state {
    SHUTDOWN_STATE_NONE = 0, // No shutdown initiated
    SHUTDOWN_STATE_APP,      // Shutdown initiated by application
    SHUTDOWN_STATE_STACK     // Shutdown initiated by MsQuic
};

typedef struct QUIC_CTX {
    pthread_mutex_t mutex;    /* mutex lock */
    HQUIC Registration;       /* MSQUIC Registration Handle */
    HQUIC Configuration;      /* MSQUIC Configuration Handle */
	HQUIC Connection;         /* MSQUIC Connection Handle */
	HQUIC Stream;             /* MSQUIC Stream Handle */
    SOCKET Socket;            /* eventfd, index of 'socket' to 'networkhandle' */
    char peer[PEER_LEN];           /* peer address */
    char* sslkeylogfile;      /* SSL key log file */
    QUIC_TLS_SECRETS tls_secrets; /* TLS secrets */
    QUIC_BUFFER *nst;         /* QUIC new session ticket, update if not NULL */
    char* recv_buf;           /* buffer to receive data */
    uint32_t recv_buf_size;   /* size of recv_buf */
    uint32_t recv_buf_offset; /* offset of unconsumed data in recv_buf */
    int is_closed;            /* Mark if connection is closed */
    enum shutdown_state shutdown_state;     /* Flag for shutdown coordination*/
} QUIC_CTX;
#endif //MSQUIC


#endif // QUICCTX_H_
