#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include <msquic.h>
#include "Socket.h"
typedef struct {
    pthread_mutex_t mutex;
	HQUIC Connection;
	HQUIC Stream;
    SOCKET Socket; // not real socket, just key for the search in aList
    char* recv_buf;
    uint32_t recv_buf_size;
    uint32_t recv_buf_offset;
    int is_closed;
} QUIC_CTX;
#endif //MSQUIC

#endif // QUICCTX_H_
