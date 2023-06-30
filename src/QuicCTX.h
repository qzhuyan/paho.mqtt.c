#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include <msquic.h>
#include "Socket.h"
typedef struct {
	HQUIC Connection;
	HQUIC Stream;
    SOCKET Socket; // not real socket, just key for the search in aList
} QUIC_CTX;
#endif //MSQUIC

#endif // QUICCTX_H_
