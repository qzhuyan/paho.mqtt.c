#ifndef QUICCTX_H_
#define QUICCTX_H_

#if defined(MSQUIC)
#include <msquic.h>
typedef struct {
	HQUIC Connection;
	HQUIC Stream;
} QUIC_CTX;
#endif //MSQUIC

#endif // QUICCTX_H_
