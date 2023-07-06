// @TODO copyright
#include "StackTrace.h"
#include "quic.h"
#include "SocketBuffer.h"
#include "Log.h"

#include <msquic.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>


#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

static void maybe_recv_complete(QUIC_CTX*);
static int QUIC_addSocket(SOCKET newSd);


/*=================================*/
/* Global QUIC handles             */
/*=================================*/
/* `Registration` Manages the execution context */
HQUIC Registration;
/* `Configuration` abstracts the configuration for a connection, */
/*   security and common QUIC Settings */
HQUIC Configuration;

/**
 * Structure to hold all socket data for this module
 */
extern mutex_type socket_mutex;

int epollfd_read = -1; /**< epoll file descriptor */

/*=================================*/
/* Global QUIC Vars                */
/*=================================*/
const QUIC_API_TABLE* MsQuic = NULL;
const QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };

const QUIC_REGISTRATION_CONFIG RegConfig = { "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY };

BOOLEAN ClientLoadConfiguration (BOOLEAN Unsecure);

// @doc: call from MQTTAsync_createWithOptions
void MSQUIC_initialize()
{
    FUNC_ENTRY;
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if(!MsQuic)
    {
        Log(TRACE_MINIMUM, -1 , "MsQuic is null, init it\n");
        if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
            printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        }
    }

    if(-1 == epollfd_read)
    {
        epollfd_read = epoll_create1(0);
    }

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    if (!ClientLoadConfiguration(TRUE)) {
        Log(LOG_ERROR, -1, "!!!!!!!client load conf failed, 0x%x!\n", Status);
        goto Error;
    }
    FUNC_EXIT;
    return;
Error:
    QUIC_outTerminate();
}

// @doc call from MQTTAsync_global_init, Global init of mqtt library
// note, global init isn't a mandatory step.
void QUIC_handleInit(int boolean)
{
    FUNC_ENTRY;
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    //
    // Open a handle to the library and get the API function table.
    //
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
    }

    FUNC_EXIT;
}

/*
 * @doc Initialize the 'quic' module,
 * call from `MQTTAsync_createWithOptions`
*/
void QUIC_outInitialize(void)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

/*
 * @doc Terminate the 'quic' module,
 * call from `MQTTAsync_terminate`
*/
void QUIC_outTerminate(void)
{
  FUNC_ENTRY;
  if (MsQuic != NULL) {
        if (Configuration != NULL) {
            MsQuic->ConfigurationClose(Configuration);
        }
        if (Registration != NULL) {
            //
            // This will block until all outstanding child objects have been
            // closed.
            //
            MsQuic->RegistrationClose(Registration);
        }
        MsQuicClose(MsQuic);
    }
}

int QUIC_getch(QUIC_CTX* q_ctx, char* c)
{
    FUNC_ENTRY;
    int rc = SOCKET_ERROR;
    if ((rc = SocketBuffer_getQueuedChar(q_ctx->Socket, c)) != SOCKETBUFFER_INTERRUPTED)
		goto exit;

    pthread_mutex_lock(&q_ctx->mutex);
    if (!q_ctx->recv_buf || q_ctx->recv_buf_size == 0)
    {
        rc = SOCKET_ERROR;
        goto exit;
    }
    // @TODO check read overflow
    *c = *(q_ctx->recv_buf + q_ctx->recv_buf_offset);
    printf("quic_get_ch %d @ %d\n", *c, q_ctx->recv_buf_offset);
    q_ctx->recv_buf_offset += 1; // one char
    pthread_mutex_unlock(&q_ctx->mutex);
    maybe_recv_complete(q_ctx);
    SocketBuffer_queueChar(q_ctx->Socket, *c);
    if (q_ctx->is_closed)
    {
        Log(LOG_ERROR, -1, "socket %d closed", q_ctx->Socket);
        rc = SOCKET_ERROR;
    }
    else
    {
        rc = 0;
    }
exit:
    FUNC_EXIT_RC(rc);
    return rc;
}

/**
 *  Attempts to read a number of bytes from a socket, non-blocking. If a previous read did not
 *  finish, then retrieve that data.
 *  @param socket the socket to read from
 *  @param bytes the number of bytes to read
 *  @param actual_len the actual number of bytes read
 *  @return completion code
 */
char *QUIC_getdata(QUIC_CTX* q_ctx, size_t bytes, size_t* actual_len, int* rc)
{
    FUNC_ENTRY;
    char* buf = NULL;
    size_t left_in_recvbuf = q_ctx->recv_buf_size - q_ctx->recv_buf_offset;
    size_t queued_bytes = 0, desired_bytes = 0;

    if (bytes == 0) // follow Socket_getdata
    {
        buf = SocketBuffer_complete(q_ctx->Socket);
        goto exit;
    }

    // get from queued data, @FIXME should get a queued data
    buf = SocketBuffer_getQueuedData(q_ctx->Socket, bytes, &queued_bytes);

    desired_bytes = bytes - queued_bytes;

    if(desired_bytes <= left_in_recvbuf)
    {
        // read all bytes
        *actual_len = desired_bytes;
        *rc = *actual_len;
    }
    else
    {
        // read less bytes
        *actual_len = left_in_recvbuf;
    }

    // read start point
    memcpy(buf+queued_bytes,
           q_ctx->recv_buf + q_ctx->recv_buf_offset,
           desired_bytes);

    // update offset for consumed
    q_ctx->recv_buf_offset += *actual_len;

    maybe_recv_complete(q_ctx);
exit:
    FUNC_EXIT_RC(*rc);
    return buf;
}

/*
**  @doc: put data to QUIC stream
 */
int QUIC_putdatas(QUIC_CTX* q_ctx, char* buf0, size_t buf0len, PacketBuffers bufs)
{
    FUNC_ENTRY;
    assert(q_ctx);
    QUIC_STATUS Status;
    HQUIC Stream = q_ctx->Stream;
    QUIC_BUFFER *SendBuffer = malloc(sizeof(QUIC_BUFFER));
    size_t SendBufferLength = 1 + bufs.count;
    SOCKET socket = q_ctx->Socket;

    // SendBuffer data
    size_t len = buf0len;
    for(int i=0; i< bufs.count; i++) {
        len += bufs.buflens[i];
    }
    SendBuffer->Length = len;

    // SendBuffer data
    char* tmpbuf = malloc(len*sizeof(char));

    if (!tmpbuf) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }
    size_t offset = 0;
    SendBuffer->Buffer = tmpbuf;
    memcpy(tmpbuf, buf0, buf0len);
    offset = buf0len;
    for(int i=0; i< bufs.count; i++) {
        memcpy(tmpbuf+offset, bufs.buffers[i], bufs.buflens[i]);
        offset += bufs.buflens[i];
    }

    Log(LOG_ERROR, -1, "QUIC_send: %d", len);

    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        goto Error;
    }

    Status = 0; //success
Error:
    FUNC_EXIT_RC(Status);
    return Status;
}

int QUIC_close(networkHandles* net, QUIC_UINT62 reasonCode)
{
    FUNC_ENTRY;
    if(net->q_ctx)
    {
        MsQuic->ConnectionShutdown(net->q_ctx->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, reasonCode);
    }
    net->quic = 0;
    net->q_ctx = NULL; // application will no longer has access to the quic ctx
    close(net->socket);
    FUNC_EXIT;
}

/* able to use GNU's getaddrinfo_a to make timeouts possible */
int QUIC_new(const char* addr, size_t addr_len, int port, networkHandles* net, long timeout)
{
    FUNC_ENTRY;
    QUIC_STATUS Status;
    char host[QUIC_MAX_SNI_LENGTH] = { 0 };

    // @TODO check return val
    strncpy(host, addr, addr_len);
    Log(LOG_ERROR, -1, "QUIC_new: host: %s, port: %d", host, port);
    //assert(net->quic);
    assert(net->q_ctx == NULL);
    net->quic = 1;
    net->ssl = 0; //@TODO set at other places?
    net->q_ctx = (QUIC_CTX *) malloc(sizeof(QUIC_CTX));
    net->q_ctx->recv_buf = NULL;
    net->q_ctx->recv_buf_size = 0;
    net->q_ctx->recv_buf_offset = 0;
    net->q_ctx->is_closed = FALSE;
    net->socket = eventfd(0, EFD_NONBLOCK);
    net->q_ctx->Socket = net->socket;
    pthread_mutex_init(&net->q_ctx->mutex, 0);

    //@TODO: check return value
    QUIC_addSocket(net->socket);
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, net->q_ctx, &net->q_ctx->Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto exit;
    }
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(net->q_ctx->Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, net->q_ctx, &net->q_ctx->Stream)))
    {
        printf("StreamOpen failed, 0x%x!\n", Status);
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->StreamStart(net->q_ctx->Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(net->q_ctx->Stream);
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(net->q_ctx->Connection, Configuration,
                                                     QUIC_ADDRESS_FAMILY_UNSPEC, host, port)))
    {
        printf("Start Configuration failed, 0x%x!\n", Status);
        goto exit;
    }

    Status = 0; //success
exit:
    FUNC_EXIT_RC(Status);
    return Status;
}

int QUIC_noPendingWrites(QSOCKET socket)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}
char* QUIC_getpeer(QSOCKET sock)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

void QUIC_addPendingWrite(QSOCKET socket)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}
void QUIC_clearPendingWrite(QSOCKET socket)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

static QUIC_writeContinue* writecontinue = NULL;
void QUIC_setWriteContinueCallback(QUIC_writeContinue* mywritecontinue)
{
    FUNC_ENTRY;
  writecontinue = mywritecontinue;
    FUNC_EXIT;
}

static QUIC_writeComplete* writecomplete = NULL;
void QUIC_setWriteCompleteCallback(QUIC_writeComplete* mywritecomplete)
{
    FUNC_ENTRY;
  writecomplete = mywritecomplete;
  FUNC_EXIT;
}

static QUIC_writeAvailable* writeavailable = NULL;
void QUIC_setWriteAvailableCallback(QUIC_writeAvailable* mywriteavailable)
{
    FUNC_ENTRY;
  writeavailable = mywriteavailable;
  FUNC_EXIT;
}

/**
 *  Returns the next socket ready for communications as indicated by epoll
 *  @param more_work flag to indicate more work is waiting, and thus a timeout value of 0 should
 *  be used for the select
 *  @param timeout the timeout to be used in ms
 *  @param rc a value other than 0 indicates an error of the returned socket
 *  @return the socket next ready, or 0 if none is ready
 */
SOCKET QUIC_getReadySocket(int more_work, int timeout_ms, mutex_type mutex, int* rc)
{
    FUNC_ENTRY;
    SOCKET socket = 0;
    struct timespec timeout;
    struct epoll_event ev;
    // @TODO lock mutex
    //pthread_mutex_lock(&q_ctx->mutex);
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += timeout_ms * 1000;

    Thread_lock_mutex(mutex);

    if(epollfd_read == -1)
    {
        // uninitialized
        *rc = 0;
    }
    else
    {
        *rc = epoll_wait(epollfd_read, &ev, 1, timeout_ms);
    }

    if (*rc == -1)
    {
        Log(LOG_ERROR, -1 , "epoll wait error: %s\n", strerror(errno));
    }
    else if(*rc >0)
    {
        uint64_t u;
        // Clear the event
        if (0 == read(ev.data.fd, &u, sizeof(uint64_t)))
            { //socket may closed
                *rc = -1;
            }
            else
            {
                Log(TRACE_MINIMUM, -1, "eventfd object is readable: %ld\n", u);
                socket = ev.data.fd;
                *rc = 0;
            }
    }
    Thread_unlock_mutex(mutex);

    FUNC_EXIT_RC(*rc);
    return socket;
}

/*
** Internals
*/
BOOLEAN
ClientLoadConfiguration(
    BOOLEAN Unsecure
    )
{
    FUNC_ENTRY;
    QUIC_SETTINGS Settings = {0};
    uint64_t IdleTimeoutMs = 10000;
    //const QUIC_BUFFER Alpn = { sizeof("MQTT") - 1, (uint8_t*)"MQTT" };
    //
    // Configures the client's idle timeout.
    //
    Settings.IdleTimeoutMs = IdleTimeoutMs;
    Settings.IsSet.IdleTimeoutMs = TRUE;

    //
    // Configures a default client configuration, optionally disabling
    // server certificate validation.
    //
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    FUNC_EXIT;
    return TRUE;
}


//
// The clients's callback for connection events from MsQuic.
//
QUIC_STATUS
QUIC_API
ClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    FUNC_ENTRY;
    QUIC_CTX *q_ctx = (QUIC_CTX *)Context;
    pthread_mutex_lock(&q_ctx->mutex);
    printf("ClientConnectionCallback: event :%d\n", Event->Type);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        printf("[conn][%p] Resumption ticket received (%u bytes):\n", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
            printf("%.2X", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        printf("\n");
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&q_ctx->mutex);
    return QUIC_STATUS_SUCCESS;
}


//
// The clients's callback for stream events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
ClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    FUNC_ENTRY;
    QUIC_STATUS ret = QUIC_STATUS_SUCCESS;
    QUIC_CTX *q_ctx = (QUIC_CTX *)Context;
    assert(q_ctx);
    pthread_mutex_lock(&q_ctx->mutex);
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        printf("[strm][%p] Data sent\n", Stream);
        QUIC_BUFFER* sendbuff = (QUIC_BUFFER *) Event->SEND_COMPLETE.ClientContext;
        free(sendbuff->Buffer);
        free(sendbuff);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        if (!Event->RECEIVE.BufferCount)
        {
            break;
        }
        q_ctx->recv_buf_size = Event->RECEIVE.TotalBufferLength;

        size_t len = 0;
        assert(q_ctx->recv_buf == NULL);
        q_ctx->recv_buf = malloc(q_ctx->recv_buf_size);
        size_t offset = 0;
        for (int i=0; i<Event->RECEIVE.BufferCount; i++) {
            memcpy(q_ctx->recv_buf+offset, Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length);
            offset += Event->RECEIVE.Buffers[i].Length;
            len += Event->RECEIVE.Buffers[i].Length;
        }
        assert(q_ctx->recv_buf_size == len);
        q_ctx->recv_buf_offset = 0;
        printf("[strm][%p] Data received: len: %d\n", Stream, Event->RECEIVE.TotalBufferLength);
        if (-1 == write(q_ctx->Socket, &(uint64_t){1}, sizeof(uint64_t)))
        {
            Log(TRACE_MINIMUM, -1, "write eventfd: %d for %d, error: %s", q_ctx->Socket, QUIC_STREAM_EVENT_RECEIVE, strerror(errno));
        }
        ret = QUIC_STATUS_PENDING;
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        printf("[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer aborted its send direction of the stream.
        //
        printf("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        printf("[strm][%p] All done\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        q_ctx->is_closed = TRUE;
        if (-1 == write(q_ctx->Socket, &(uint64_t){1}, sizeof(uint64_t)))
        {
            Log(TRACE_MINIMUM, -1, "write eventfd: %d for %d, error: %s", q_ctx->Socket, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE, strerror(errno));
        }
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&q_ctx->mutex);
    return ret;
}


void maybe_recv_complete(QUIC_CTX *q_ctx)
{
    if(!q_ctx)
    {
        return;
    }
    // @TODO this lock may not be necessary since
    // stream callback with receive event shall not be triggered
    // due to partial receive
    pthread_mutex_lock(&q_ctx->mutex);
    if(q_ctx->recv_buf_offset == q_ctx->recv_buf_size)
    {
        printf("receving complete %d\n", q_ctx->recv_buf_size);
        free(q_ctx->recv_buf);
        q_ctx->recv_buf_offset = 0;
        q_ctx->recv_buf = NULL;
        q_ctx->recv_buf_size = 0;
    }
    MsQuic->StreamReceiveComplete(q_ctx->Stream, q_ctx->recv_buf_size);
    pthread_mutex_unlock(&q_ctx->mutex);
}

/**
 * Add a socket to the list of socket to check with poll()
 * @param newSd the new event socket to add
 */
int QUIC_addSocket(SOCKET newSd)
{
	int rc = 0;

	FUNC_ENTRY;
	Thread_lock_mutex(socket_mutex);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = newSd;
    if (-1 == epoll_ctl(epollfd_read, EPOLL_CTL_ADD, newSd, &ev)) {
        Log(LOG_FATAL, -1, "epoll_ctl error: %s\n", strerror(errno));
    }
    Log(TRACE_MINIMUM, -1, "epoll_ctl add socket success\n");
exit:
	Thread_unlock_mutex(socket_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}
