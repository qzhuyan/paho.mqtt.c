#ifndef QUIC_H_
#define QUIC_H_

#include <stdint.h>
#include "mutex_type.h" /* Needed for mutex_type */
#include <msquic.h>

typedef HQUIC SOCKET;

/*
 * Network write buffers for an MQTT packet
 */
typedef struct
{
	int count;         /**> number of buffers/buflens/frees */
	char** buffers;    /**> array of byte buffers */
	size_t* buflens;   /**> array of lengths of buffers */
	int* frees;        /**> array of flags indicating whether each buffer needs to be freed */
	uint8_t mask[4];   /**> websocket mask used to mask the buffer data, if any */
} PacketBuffers;


void QUIC_outInitialize(void);
void QUIC_outTerminate(void);

SOCKET QUIC_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc);

int QUIC_getch(SOCKET socket, char* c);
char *QUIC_getdata(SOCKET socket, size_t bytes, size_t* actual_len, int* rc);
int QUIC_putdatas(SOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs);
int QUIC_close(SOCKET socket);

#if defined(__GNUC__) && defined(__linux__)
/* able to use GNU's getaddrinfo_a to make timeouts possible */
int QUIC_new(const char* addr, size_t addr_len, int port, SOCKET* socket, long timeout);
#else
int QUIC_new(const char* addr, size_t addr_len, int port, SOCKET* socket);
#endif

int QUIC_noPendingWrites(SOCKET socket);
char* QUIC_getpeer(SOCKET sock);

void QUIC_addPendingWrite(SOCKET socket);
void QUIC_clearPendingWrite(SOCKET socket);

typedef void QUIC_writeContinue(SOCKET socket);
void QUIC_setWriteContinueCallback(QUIC_writeContinue*);

typedef void QUIC_writeComplete(SOCKET socket, int rc);
void QUIC_setWriteCompleteCallback(QUIC_writeComplete*);

typedef void QUIC_writeAvailable(SOCKET socket);
void QUIC_setWriteAvailableCallback(QUIC_writeAvailable*);

#endif // QUIC_H_
