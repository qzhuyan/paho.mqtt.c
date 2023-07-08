#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include <msquic.h>
#include "Socket.h"
typedef struct QUIC_CTX {
    pthread_mutex_t mutex;    /* mutex lock */
	HQUIC Connection;         /* MSQUIC Connection Handle */
	HQUIC Stream;             /* MSQUIC Stream Handle */
    SOCKET Socket;            /* eventfd, index of 'socket' to 'networkhandle' */
    char* recv_buf;           /* buffer to receive data */
    uint32_t recv_buf_size;   /* size of recv_buf */
    uint32_t recv_buf_offset; /* offset of unconsumed data in recv_buf */
    int is_closed;            /* Mark if connection is closed */
} QUIC_CTX;
#endif //MSQUIC

#endif // QUICCTX_H_
