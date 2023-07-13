#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include <msquic.h>
#include "Socket.h"

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
    char* recv_buf;           /* buffer to receive data */
    uint32_t recv_buf_size;   /* size of recv_buf */
    uint32_t recv_buf_offset; /* offset of unconsumed data in recv_buf */
    int is_closed;            /* Mark if connection is closed */
    enum shutdown_state shutdown_state;     /* Flag for shutdown coordination*/
} QUIC_CTX;
#endif //MSQUIC


#endif // QUICCTX_H_
