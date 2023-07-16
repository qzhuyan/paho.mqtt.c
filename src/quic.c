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
/* `Configuration` abstracts the configuration for a connection, */
/*   security and common QUIC Settings */


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

BOOLEAN ClientLoadConfiguration (QUIC_CTX* q_ctx, MQTTClient_SSLOptions *sslopts);

// @doc: call from MQTTAsync_createWithOptions
void MSQUIC_initialize()
{
    FUNC_ENTRY;
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if(!MsQuic)
    {
        Log(TRACE_MINIMUM, -1 , "MsQuic is null, init it");
        if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
            Log(LOG_FATAL, -1, "MsQuicOpen2 failed, 0x%x!", Status);
        }
    }

    if(-1 == epollfd_read)
    {
        epollfd_read = epoll_create1(0);
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
    if(NULL == MsQuic)
    {
        if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
            Log(LOG_FATAL, -1, "MsQuicOpen2 failed, 0x%x!", Status);
        }
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
    MSQUIC_initialize();
    FUNC_EXIT;
}

/*
 * @doc Terminate the 'quic' connection.
 * call from `MQTTAsync_terminate`
*/
void QUIC_outTerminate(void)
{
  FUNC_ENTRY;
  FUNC_EXIT;
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
    Log(TRACE_MAXIMUM, -1, "quic_get_ch %d @ %d\n", *c, q_ctx->recv_buf_offset);
    q_ctx->recv_buf_offset += 1; // one char
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
    pthread_mutex_unlock(&q_ctx->mutex);
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

    pthread_mutex_lock(&q_ctx->mutex);
    maybe_recv_complete(q_ctx);
    pthread_mutex_unlock(&q_ctx->mutex);
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
        Log(TRACE_MINIMUM, -1, "SendBuffer allocation failed!\n");
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
        Log(TRACE_MINIMUM, -1, "StreamSend failed, 0x%x!\n", Status);
        free(SendBuffer->Buffer);
        free(SendBuffer);
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
    QUIC_CTX *q_ctx = net->q_ctx;
    if(net->q_ctx)
    {
        Log(TRACE_MINIMUM, -1, "QUIC_closing q_ctx %p\n", q_ctx);

        // application will no longer has access to the ctx
        // from networkHandles
        net->q_ctx = NULL;
        net->quic = 0;

        pthread_mutex_lock(&q_ctx->mutex);

        assert(q_ctx->shutdown_state != SHUTDOWN_STATE_APP);

        if (SHUTDOWN_STATE_NONE == q_ctx->shutdown_state)
        {
            HQUIC Registration = q_ctx->Registration;
            Log(TRACE_MINIMUM, -1, "trigger connection shutdown in QUIC_close: %p\n", q_ctx->Connection);
            q_ctx->shutdown_state = SHUTDOWN_STATE_APP;
            MsQuic->ConnectionShutdown(q_ctx->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, reasonCode);
            pthread_mutex_unlock(&q_ctx->mutex);
            // long blocking, wait for complete clean
            MsQuic->RegistrationClose(Registration);
        }
        else if(SHUTDOWN_STATE_STACK == q_ctx->shutdown_state)
        {
            // Already closed by MsQuic Stack
            Log(TRACE_MINIMUM, -1, "Free q_ctx in QUIC_close: %p\n", q_ctx);
            close(q_ctx->Socket);
            pthread_mutex_unlock(&q_ctx->mutex);
            pthread_mutex_destroy(&q_ctx->mutex);
            MsQuic->ConfigurationClose(q_ctx->Configuration);
            MsQuic->RegistrationClose(q_ctx->Registration);
            printf("registration closed\n");
            free(q_ctx);
        }

    }

    FUNC_EXIT;
}

void EncodeHexBuffer(uint8_t* Buffer,
                     uint8_t BufferLen,
                     char* HexString);
inline
void
EncodeHexBuffer(
    _In_reads_(BufferLen) uint8_t* Buffer,
    _In_ uint8_t BufferLen,
    _Out_writes_bytes_(2*BufferLen) char* HexString
    )
{
    #define HEX_TO_CHAR(x) ((x) > 9 ? ('a' + ((x) - 10)) : '0' + (x))
    for (uint8_t i = 0; i < BufferLen; i++) {
        HexString[i*2]     = HEX_TO_CHAR(Buffer[i] >> 4);
        HexString[i*2 + 1] = HEX_TO_CHAR(Buffer[i] & 0xf);
    }
}


void
dump_sslkeylogfile(_In_z_ const char *FileName,
                   _In_ QUIC_TLS_SECRETS TlsSecrets)
{
  FILE *File = NULL;
#ifdef _WIN32
  if (fopen_s(&File, FileName, "ab"))
    {
      printf("Failed to open sslkeylogfile %s\n", FileName);
      return;
    }
#else
  File = fopen(FileName, "ab");
#endif

  if (File == NULL)
    {
      printf("Failed to open sslkeylogfile %s\n", FileName);
      return;
    }
  if (fseek(File, 0, SEEK_END) == 0 && ftell(File) == 0)
    {
      fprintf(File, "# TLS 1.3 secrets log file\n");
    }
  char
      ClientRandomBuffer[(2 * sizeof(((QUIC_TLS_SECRETS *)NULL)->ClientRandom))
                         + 1]
      = { 0 };
  char TempHexBuffer[(2 * QUIC_TLS_SECRETS_MAX_SECRET_LEN) + 1] = { 0 };
  if (TlsSecrets.IsSet.ClientRandom)
    {
      EncodeHexBuffer(TlsSecrets.ClientRandom,
                      (uint8_t)sizeof(TlsSecrets.ClientRandom),
                      ClientRandomBuffer);
    }

  if (TlsSecrets.IsSet.ClientEarlyTrafficSecret)
    {
      EncodeHexBuffer(TlsSecrets.ClientEarlyTrafficSecret,
                      TlsSecrets.SecretLength,
                      TempHexBuffer);
      fprintf(File,
              "CLIENT_EARLY_TRAFFIC_SECRET %s %s\n",
              ClientRandomBuffer,
              TempHexBuffer);
    }

  if (TlsSecrets.IsSet.ClientHandshakeTrafficSecret)
    {
      EncodeHexBuffer(TlsSecrets.ClientHandshakeTrafficSecret,
                      TlsSecrets.SecretLength,
                      TempHexBuffer);
      fprintf(File,
              "CLIENT_HANDSHAKE_TRAFFIC_SECRET %s %s\n",
              ClientRandomBuffer,
              TempHexBuffer);
    }

  if (TlsSecrets.IsSet.ServerHandshakeTrafficSecret)
    {
      EncodeHexBuffer(TlsSecrets.ServerHandshakeTrafficSecret,
                      TlsSecrets.SecretLength,
                      TempHexBuffer);
      fprintf(File,
              "SERVER_HANDSHAKE_TRAFFIC_SECRET %s %s\n",
              ClientRandomBuffer,
              TempHexBuffer);
    }

  if (TlsSecrets.IsSet.ClientTrafficSecret0)
    {
      EncodeHexBuffer(TlsSecrets.ClientTrafficSecret0,
                      TlsSecrets.SecretLength,
                      TempHexBuffer);
      fprintf(File,
              "CLIENT_TRAFFIC_SECRET_0 %s %s\n",
              ClientRandomBuffer,
              TempHexBuffer);
    }

  if (TlsSecrets.IsSet.ServerTrafficSecret0)
    {
      EncodeHexBuffer(TlsSecrets.ServerTrafficSecret0,
                      TlsSecrets.SecretLength,
                      TempHexBuffer);
      fprintf(File,
              "SERVER_TRAFFIC_SECRET_0 %s %s\n",
              ClientRandomBuffer,
              TempHexBuffer);
    }

  fflush(File);
  fclose(File);
}


/**
 *   Create and start new QUIC connection
 *   @return completion code 0=good, SOCKET_ERROR=fail
 */
int QUIC_new(const char* addr, size_t addr_len, int port, networkHandles* net, MQTTClient_SSLOptions *sslopts, long timeout)
{
    FUNC_ENTRY;
    QUIC_STATUS Status;
    char host[QUIC_MAX_SNI_LENGTH] = { 0 };


    // @TODO check return val
    strncpy(host, addr, addr_len);
    Log(LOG_ERROR, -1, "QUIC_new: host: %s, port: %d", host, port);

    if (net->q_ctx)
    {
        Log(TRACE_MINIMUM, -1 , "QUIC_new: closing old %p\n", net->q_ctx, net->q_ctx->Socket);
        QUIC_close(net, 9);// @TODO reason code
    }
    net->quic = 1;
    net->ssl = 0; //@TODO set at other places?
    net->q_ctx = (QUIC_CTX *) malloc(sizeof(QUIC_CTX));
    net->q_ctx->recv_buf = NULL;
    net->q_ctx->recv_buf_size = 0;
    net->q_ctx->recv_buf_offset = 0;
    net->q_ctx->is_closed = FALSE;
    net->q_ctx->shutdown_state = SHUTDOWN_STATE_NONE;
    net->socket = eventfd(0, EFD_NONBLOCK);
    net->q_ctx->Socket = net->socket;
    pthread_mutex_init(&net->q_ctx->mutex, 0);

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &net->q_ctx->Registration))) {
        Log(LOG_FATAL, -1, "RegistrationOpen failed, 0x%x!\n", Status);
        goto exit;
    }

    //@TODO: check return value
    QUIC_addSocket(net->socket);
    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(net->q_ctx->Registration, ClientConnectionCallback, net->q_ctx, &net->q_ctx->Connection))) {
        Log(TRACE_MINIMUM, -1, "ConnectionOpen failed, 0x%x!\n", Status);
        goto exit;
    }
    Log(TRACE_MINIMUM, -1, "QUIC_new: conn: %p", net->q_ctx->Connection);


    // @TODO: condition set
    Status = MsQuic->SetParam(
        net->q_ctx->Connection,
        QUIC_PARAM_CONN_TLS_SECRETS,
        sizeof(QUIC_TLS_SECRETS), &net->q_ctx->tls_secrets);
    if (QUIC_SUCCEEDED(Status)) {
        Log(TRACE_MINIMUM, -1, "set QUIC_PARAM_CONN_TLS_SECRETS success");
    }

    if (QUIC_FAILED(Status = MsQuic->StreamOpen(net->q_ctx->Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, net->q_ctx, &net->q_ctx->Stream)))
    {
        Log(TRACE_MINIMUM, -1, "StreamOpen failed, 0x%x!\n", Status);
        goto exit;
    }

    // Load configuration, @TODO: Load SSL configuration
    if (!ClientLoadConfiguration(net->q_ctx, sslopts)) {
        Log(LOG_ERROR, -1, "!!!!!!!client load conf failed\n");
        Status = SOCKET_ERROR; // @FIXME we may not use Status
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->StreamStart(net->q_ctx->Stream, QUIC_STREAM_START_FLAG_NONE))) {
        Log(TRACE_MINIMUM, -1, "StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(net->q_ctx->Stream);
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(net->q_ctx->Connection, net->q_ctx->Configuration,
                                                     QUIC_ADDRESS_FAMILY_UNSPEC, host, port)))
    {
        Log(TRACE_MINIMUM, -1, "Start Configuration failed, 0x%x!\n", Status);
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
    QUIC_CTX* q_ctx,
    MQTTClient_SSLOptions *sslopts
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
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (!sslopts->enableServerCertAuth) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    CredConfig.CertificateFile->CertificateFile = sslopts->trustStore;
    CredConfig.CertificateFile->PrivateKeyFile = sslopts->privateKey;
    CredConfig.CaCertificateFile = sslopts->CApath;

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(q_ctx->Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &q_ctx->Configuration))) {
        Log(TRACE_MINIMUM, -1, "ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(q_ctx->Configuration, &CredConfig))) {
        Log(TRACE_MINIMUM, -1, "ConfigurationLoadCredential failed, 0x%x!\n", Status);
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
    Log(TRACE_MINIMUM, -1, "ClientConnectionCallback: event :%d", Event->Type);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        Log(TRACE_MINIMUM, -1, "[conn][%p] Connected\n", Connection);
        dump_sslkeylogfile("/tmp/SSLKEYLOGFILE", q_ctx->tls_secrets);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            Log(TRACE_MINIMUM, -1, "[conn][%p] Successfully shut down on idle.", Connection);
        } else {
            Log(TRACE_MINIMUM, -1, "[conn][%p] Shut down by transport, 0x%x", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        Log(TRACE_MINIMUM, -1, "[conn][%p] Shut down by peer, 0x%llu", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        Log(TRACE_MINIMUM, -1, "[conn][%p] All done", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        q_ctx->Connection = NULL;
        if (SHUTDOWN_STATE_APP == q_ctx->shutdown_state)
        {
            // Already shutdown by application
            // safe to unlock
            Log(TRACE_MINIMUM, -1, "[conn][%p] Connection already closed by app: %p", Connection, q_ctx);
            pthread_mutex_unlock(&q_ctx->mutex);
            close(q_ctx->Socket);
            MsQuic->ConfigurationClose(q_ctx->Configuration);
            // @NOTE never call RegistrationClose from here
            pthread_mutex_destroy(&q_ctx->mutex);
            free(q_ctx);
            goto exit;
        } else {
            q_ctx->shutdown_state = SHUTDOWN_STATE_STACK;
            pthread_mutex_unlock(&q_ctx->mutex);
        }

        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        Log(TRACE_MINIMUM, "[conn][%p] Resumption ticket received (%u bytes):", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
           Log(TRACE_MAXIMUM, -1, "%.2X\n", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&q_ctx->mutex);
exit:
    FUNC_EXIT;
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
        Log(TRACE_MINIMUM, -1, "[strm][%p] Data sent\n", Stream);
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
        Log(TRACE_MINIMUM, -1, "[strm][%p] Data received: len: %d\n", Stream, Event->RECEIVE.TotalBufferLength);
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
        Log(TRACE_MINIMUM, -1, "[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer aborted its send direction of the stream.
        //
        Log(TRACE_MINIMUM, -1, "[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        Log(TRACE_MINIMUM, -1, "[strm][%p] All done\n", Stream);
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
    FUNC_EXIT_RC(ret);
    return ret;
}


void maybe_recv_complete(QUIC_CTX *q_ctx)
{
    if(!q_ctx)
    {
        return;
    }
    if(q_ctx->recv_buf_offset == q_ctx->recv_buf_size)
    {
        Log(TRACE_MINIMUM, -1, "receving complete %d for %p",
            q_ctx->recv_buf_size, q_ctx->Stream);
        free(q_ctx->recv_buf);
        q_ctx->recv_buf_offset = 0;
        q_ctx->recv_buf = NULL;
        q_ctx->recv_buf_size = 0;
        MsQuic->StreamReceiveComplete(q_ctx->Stream, q_ctx->recv_buf_size);
    }
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
