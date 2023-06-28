#include "StackTrace.h"
#include "quic.h"
#include "Log.h"
#include <msquic.h>

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

const QUIC_API_TABLE* MsQuic;

const QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };
//
// The QUIC handle to the registration object. This is the top level API object
// that represents the execution context for all work done by MsQuic on behalf
// of the app.
//
HQUIC Registration;
HQUIC Configuration;


// @TODO they should be put in to ctx
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
uint32_t recv_buf_size = 0;
uint32_t recv_buf_offset = 0;
HQUIC recv_pending_stream = NULL;
char* recv_buf = NULL;

const QUIC_REGISTRATION_CONFIG RegConfig = { "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY };

BOOLEAN ClientLoadConfiguration (BOOLEAN Unsecure);

//void ClientSend(HQUIC Stream);

// @doc: init msquic stack
void MSQUIC_initialize()
{
    FUNC_ENTRY;
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    //
    // Open a handle to the library and get the API function table.
    //
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        goto Error;
    }

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("RegistrationOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    if (!ClientLoadConfiguration(TRUE)) {
        printf("!!!!!!!client load conf failed, 0x%x!\n", Status);
        goto Error;
    }
    FUNC_EXIT;
    return;
Error:
    QUIC_outTerminate();
}

void QUIC_handleInit(int boolean)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

void QUIC_outInitialize(void)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

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

QSOCKET QUIC_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

int QUIC_getch(QUIC_CTX* q_ctx, char* c)
{
    FUNC_ENTRY;
    int res = -1;
    *c = *(recv_buf+recv_buf_offset);
    printf("quic_get_ch %d @ %d", *c, recv_buf_offset);
    //MsQuic->StreamReceiveComplete(q_ctx->Stream, 1);
    recv_buf_offset += 1; // one char
    if(recv_buf_offset == recv_buf_size)
    {
        recv_buf_offset = 0;
    }
    res = 0;
    FUNC_EXIT_RC(res);
    return res;
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
    char* res = NULL;
    if(bytes < recv_buf_size - recv_buf_offset)
    {
        *actual_len = bytes;
        *rc = *actual_len;
    }
    else
    {
        *actual_len = recv_buf_size - recv_buf_offset;
    }

    res = recv_buf+recv_buf_offset;

    recv_buf_offset += *actual_len;

    if (recv_buf_offset == recv_buf_size)
    {
        MsQuic->StreamReceiveComplete(q_ctx->Stream, recv_buf_size);
        recv_buf_offset = 0;
        recv_buf = NULL;
    }
    

    FUNC_EXIT_RC(*rc);
    return res;
}


int QUIC_putdatas(QSOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs)
{
    FUNC_ENTRY;
    QUIC_STATUS Status;
    HQUIC Stream = socket;
    QUIC_BUFFER* SendBufferRaw = NULL;
    QUIC_BUFFER* SendBuffer = NULL;
    size_t SendBufferLength = 1 + bufs.count;

    printf("total size: %ld\n", SendBufferLength*sizeof(QUIC_BUFFER));
    SendBufferRaw = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) * SendBufferLength);
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }
    SendBuffer = SendBufferRaw;
    // @TODO don't forget to free it
    char* tmpbuf = malloc(buf0len*sizeof(char));
    memcpy(tmpbuf, buf0, buf0len);
    SendBuffer[0].Buffer = tmpbuf;
    SendBuffer[0].Length = buf0len;
    for(int i=0; i< bufs.count; i++) {
        SendBuffer[i+1].Buffer = bufs.buffers[i];
        SendBuffer[i+1].Length = bufs.buflens[i];
    }
    Log(LOG_ERROR, -1, "QUIC_send: %d", SendBufferLength);

    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, SendBufferLength, QUIC_SEND_FLAG_NONE, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        goto Error;
    }

    Status = 0; //success
Error:
    FUNC_EXIT_RC(Status);
    return Status;
}

int QUIC_close(QSOCKET socket)
{
    FUNC_ENTRY;
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
    net->ssl = 3; //@TODO set at other places?
    net->q_ctx = (QUIC_CTX *) malloc(sizeof(QUIC_CTX));

    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, NULL, &net->q_ctx->Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        goto exit;
    }
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(net->q_ctx->Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, NULL, &net->q_ctx->Stream)))
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


HQUIC QUIC_wait_for_readable(unsigned long timeout_ms)
{
    FUNC_ENTRY;
    QSOCKET res = 0;
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += timeout_ms * 1000;
    pthread_mutex_lock(&mutex);
    if (recv_pending_stream == NULL)
    {
        int result = pthread_cond_timedwait(&cond, &mutex, &timeout);
        if(result == ETIMEDOUT)
        {
            res = -1; // fake socket
        }
        else if(result == 0)
        {
            assert(recv_pending_stream != NULL);
            printf("pending recv on Stream %p \n", recv_pending_stream);
            res = recv_pending_stream;
            MsQuic->StreamReceiveComplete(recv_pending_stream, 0);
            recv_pending_stream = NULL;
        }
        else
        {
            printf("pthread_cond_timedwait failed, %d\n", result);
            res = -1;
        }
    }
    pthread_mutex_unlock(&mutex);
    FUNC_EXIT_RC(res);
    return res;
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
    uint64_t IdleTimeoutMs = 1000;
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
    UNREFERENCED_PARAMETER(Context);
    printf("ClientConnectionCallback: event :%d\n", Event->Type);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
        //ClientSend(Connection);
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
    UNREFERENCED_PARAMETER(Context);
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        free(Event->SEND_COMPLETE.ClientContext);
        printf("[strm][%p] Data sent\n", Stream);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        printf("[strm][%p] Data received: len: %d\n", Stream, Event->RECEIVE.TotalBufferLength);
        if (!Event->RECEIVE.BufferCount)
        {
            break;
        }
        pthread_mutex_lock(&mutex);
        /* recv_buf[0].Buffer = Event->RECEIVE.Buffers[0].Buffer; */
        /* recv_buf[0].Length = Event->RECEIVE.Buffers[0].Length; */
        /* recv_buf[1].Buffer = Event->RECEIVE.Buffers[1].Buffer; */
        /* recv_buf[1].Length = Event->RECEIVE.Buffers[1].Length; */
        recv_buf_size = Event->RECEIVE.TotalBufferLength;
        size_t len = 0;

        assert(recv_buf == NULL);
        recv_buf = malloc(recv_buf_size);
        size_t offset = 0;
        for (int i=0; i<Event->RECEIVE.BufferCount; i++) {
            memcpy(recv_buf+offset, Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length);
            offset += Event->RECEIVE.Buffers[i].Length;
            len += Event->RECEIVE.Buffers[i].Length;
        }
        assert(recv_buf_size == len);
        recv_pending_stream = Stream;
        recv_buf_offset = 0;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
        return QUIC_STATUS_PENDING;
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
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


/*
void
ClientSend(
    _In_ HQUIC Connection
    )
{
    FUNC_ENTRY;
    QUIC_STATUS Status;
    HQUIC Stream = NULL;
    uint8_t* SendBufferRaw;
    QUIC_BUFFER* SendBuffer;

    uint32_t SendBufferLength = 100;

    //
    // Create/allocate a new bidirectional stream. The stream is just allocated
    // and no QUIC stream identifier is assigned until it's started.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, NULL, &Stream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        goto Error;
    }

    printf("[strm][%p] Starting...\n", Stream);

    //
    // Starts the bidirectional stream. By default, the peer is not notified of
    // the stream being started until data is sent on the stream.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        goto Error;
    }

    //
    // Allocates and builds the buffer to send over the stream.
    //
    SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + SendBufferLength);
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }
    SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
    SendBuffer->Length = SendBufferLength;

    printf("[strm][%p] Sending data...\n", Stream);

    //
    // Sends the buffer over the stream. Note the FIN flag is passed along with
    // the buffer. This indicates this is the last buffer on the stream and the
    // the stream is shut down (in the send direction) immediately after.
    //
    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
        free(SendBufferRaw);
        goto Error;
    }

Error:

    if (QUIC_FAILED(Status)) {
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}
*/
