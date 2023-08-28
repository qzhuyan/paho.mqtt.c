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
#include "StackTrace.h"
#include "Quic.h"
#include "SocketBuffer.h"
#include "Log.h"

#include <msquic.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <openssl/err.h>

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

static void maybe_recv_complete(QUIC_CTX*);

static int QUIC_addSocket(SOCKET newSd);

extern int Socket_addSocket(SOCKET newSd);

/*=================================*/
/* Global QUIC handles             */
/*=================================*/
/**
 * Structure to hold all socket data for this module
 */
extern mutex_type socket_mutex;

#if defined(MSQUIC_USE_EPOLL)
int epollfd_read = -1; /**< epoll file descriptor */
#endif

/*=================================*/
/* Global QUIC Vars                */
/*=================================*/
const QUIC_API_TABLE* MsQuic = NULL;
const QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };

const QUIC_REGISTRATION_CONFIG RegConfig = { "default", QUIC_EXECUTION_PROFILE_LOW_LATENCY };

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

#if defined(MSQUIC_USE_EPOLL)
    if(-1 == epollfd_read)
    {
        epollfd_read = epoll_create1(0);
    }
#endif
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
    MSQUIC_initialize();
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
    if (!q_ctx)
    {
        rc = SOCKETBUFFER_INTERRUPTED;
        FUNC_EXIT_RC(rc);
        return rc;
    }
    if ((rc = SocketBuffer_getQueuedChar(q_ctx->Socket, c)) != SOCKETBUFFER_INTERRUPTED)
    {
        Log(TRACE_MINIMUM, -1, "quic_get_ch %d @ Queue\n", *c);
		goto exit;
    }

    pthread_mutex_lock(&q_ctx->mutex);
    if (!q_ctx->recv_buf || q_ctx->recv_buf_size == 0)
    {
        rc = SOCKET_ERROR;
        goto exit;
    }
    *c = *(q_ctx->recv_buf + q_ctx->recv_buf_offset);
    Log(TRACE_MINIMUM, -1, "quic_get_ch %d @ %d\n", *c, q_ctx->recv_buf_offset);
    q_ctx->recv_buf_offset += 1; // one char
    assert(q_ctx->recv_buf_offset <= q_ctx->recv_buf_size);
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

    // Get (consumed len) from queued data
    buf = SocketBuffer_getQueuedData(q_ctx->Socket, bytes, &queued_bytes);
    *actual_len = queued_bytes;

    if (*actual_len > 0)
    {
        Log(TRACE_MINIMUM, -1, "we have buffered %ld", *actual_len);
    }

    // Precompute how many bytes we want to consume from recv_buf
    desired_bytes = bytes - *actual_len;

    if (desired_bytes == left_in_recvbuf)
    {
        // consume all buffers, no update on desired_bytes
        Log(TRACE_MINIMUM, -1, "we have %ld", left_in_recvbuf);
    }
    else if (desired_bytes < left_in_recvbuf)
    {
        // consume partial buffers, no update on desired_bytes
        Log(TRACE_MINIMUM, -1, "we have left %ld, consumed %ld", left_in_recvbuf-desired_bytes, desired_bytes);
        // Trigger another read for left data in the same read thread.
        if (-1 == write(q_ctx->Socket, &(uint64_t){1}, sizeof(uint64_t)))
        {
            // @TODO error handling, maybe just crash
            Log(TRACE_MINIMUM, -1, "write eventfd: %d for %d, error: %s", q_ctx->Socket, QUIC_STREAM_EVENT_RECEIVE, strerror(errno));
        }
    }
    else
    {
        // interrupted recv, need more data than we have in recv_buf
        Log(TRACE_MINIMUM, -1, "we have %ld but need %ld", left_in_recvbuf, desired_bytes);
        desired_bytes = left_in_recvbuf;
    }

    // @TODO check return val
    // read start point,
    memcpy(buf+queued_bytes,
           q_ctx->recv_buf + q_ctx->recv_buf_offset,
           desired_bytes);

    *actual_len += desired_bytes;
    if (*actual_len == bytes) // is it a dummy call if SocketBuffer is empty?
    {
        Log(TRACE_MINIMUM, -1, "SocketBuffer sock: %d complete with %ld",
            q_ctx->Socket, *actual_len);
        SocketBuffer_complete(q_ctx->Socket);
    }
    else
    {
        // tracking bytes consumed
        SocketBuffer_interrupted(q_ctx->Socket, *actual_len);
    }

    // update offset for consumed
    q_ctx->recv_buf_offset += desired_bytes;

    // Last set rc val
    *rc = desired_bytes;
    // @FIXME this lock is not necessary? or should be moved up?
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
        if (bufs.buffers[i] != NULL) {
            memcpy(tmpbuf+offset, bufs.buffers[i], bufs.buflens[i]);
            offset += bufs.buflens[i];
        }
    }

    Log(TRACE_MINIMUM, -1, "QUIC_send: %d", len);

    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1,
                                                QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_ALLOW_0_RTT,
                                                SendBuffer)))
    {
        Log(TRACE_MINIMUM, -1, "StreamSend failed, 0x%x!\n", Status);
        free(SendBuffer->Buffer);
        free(SendBuffer);
        Status = -1; // Socket Error
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
    // @TODO (feature) close with reasonCode
    if(net->q_ctx)
    {
        Log(TRACE_MINIMUM, -1, "QUIC_closing q_ctx %p\n", q_ctx);

        // application will no longer has access to the ctx
        // from networkHandles
        // @FIXME: check if thread safe here?
        net->q_ctx = NULL;

        pthread_mutex_lock(&q_ctx->mutex);
        assert(q_ctx->shutdown_state != SHUTDOWN_STATE_APP);

        if (SHUTDOWN_STATE_NONE == q_ctx->shutdown_state)
        {
            HQUIC Registration = q_ctx->Registration;
            Log(TRACE_MINIMUM, -1, "trigger connection shutdown in QUIC_close: %p\n", q_ctx->Connection);
            if (q_ctx->Stream)
            {
                // Gracefully Shutdown control stream
                // According to protocol, shutdown control stream also means connection shutdown
                MsQuic->StreamShutdown(q_ctx->Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, reasonCode);
                pthread_mutex_unlock(&q_ctx->mutex);
            }
            else
            {
                q_ctx->shutdown_state = SHUTDOWN_STATE_APP;
                MsQuic->ConnectionShutdown(q_ctx->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, reasonCode);
                pthread_mutex_unlock(&q_ctx->mutex);
                // long blocking, wait for complete clean
                MsQuic->RegistrationClose(Registration);
                Log(TRACE_MINIMUM, -1, "registration closed %p\n", q_ctx);
            }
        }
        else if(SHUTDOWN_STATE_STACK == q_ctx->shutdown_state)
        {
            // Already closed by MsQuic Stack
            Log(TRACE_MINIMUM, -1, "Free q_ctx in QUIC_close: %p\n", q_ctx);
            if(q_ctx->recv_buf)
            {
                Log(LOG_ERROR, -1, "Closing Socket with unconsumed data: %p\n", q_ctx);
                free(q_ctx->recv_buf);
            }
            // q_ctx->Socket is event socket.
            Socket_close(q_ctx->Socket);
            free(q_ctx->recv_buf);
            q_ctx->recv_buf = NULL;
            pthread_mutex_unlock(&q_ctx->mutex);
            pthread_mutex_destroy(&q_ctx->mutex);
            MsQuic->ConfigurationClose(q_ctx->Configuration);
            MsQuic->RegistrationClose(q_ctx->Registration);
            Log(TRACE_MINIMUM, -1, "registration closed %p\n", q_ctx);
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
    int use_0rtt = FALSE;

    if (net->q_ctx)
    {
        Log(TRACE_MINIMUM, -1 , "QUIC_new: closing old %p\n", net->q_ctx, net->q_ctx->Socket);
        QUIC_close(net, 9);// @TODO reason code
    }

    net->quic = 1;
    net->ssl = 0; //@TODO set at other places?
    net->q_ctx = (QUIC_CTX *) malloc(sizeof(QUIC_CTX));
    memset(net->q_ctx, 0, sizeof(QUIC_CTX));
    net->q_ctx->recv_buf = NULL;
    net->q_ctx->recv_buf_size = 0;
    net->q_ctx->recv_buf_offset = 0;
    net->q_ctx->is_closed = FALSE;
    net->q_ctx->shutdown_state = SHUTDOWN_STATE_NONE;
    net->socket = eventfd(0, EFD_NONBLOCK);
    net->q_ctx->Socket = net->socket;
    net->q_ctx->Stream = 0;
    net->q_ctx->Connection = 0;
    net->q_ctx->sslkeylogfile = getenv("SSLKEYLOGFILE");
    if (sslopts->zero_rtt != ZERO_RTT_DISABLED)
    {
        if (!sslopts->session_ticket && sslopts->zero_rtt == ZERO_RTT_AUTO)
        {
            sslopts->session_ticket = malloc(sizeof(QUIC_BUFFER));
            sslopts->session_ticket->Buffer = NULL;
        }
        if (!sslopts->session_ticket)
        {
            Log(LOG_ERROR, -1, "QUIC_new: failed to set session ticket buffer, \
                                either not in AUTO mode or OOM\n");
            goto exit;
        }
        net->q_ctx->nst = sslopts->session_ticket;
    }
    else
    {
        net->q_ctx->nst = NULL;
    }

    pthread_mutex_init(&net->q_ctx->mutex, 0);

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &net->q_ctx->Registration))) {
        Log(LOG_FATAL, -1, "RegistrationOpen failed, 0x%x!\n", Status);
        goto exit;
    }


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


    // Load configuration
    if (!ClientLoadConfiguration(net->q_ctx, sslopts)) {
        Log(LOG_ERROR, -1, "Client load conf failed\n");
        Status = SOCKET_ERROR; // @FIXME we may not use Status
        goto exit;
    }

    char* host  = (char *)malloc((addr_len + 1) * sizeof(char));
    strncpy(host, addr, addr_len);
    host[addr_len] = '\0';

    // New session ticket for connection resume
    if (sslopts->zero_rtt && sslopts->session_ticket && sslopts->session_ticket->Buffer)
    {
        if (QUIC_FAILED(Status
                          = MsQuic->SetParam(net->q_ctx->Connection,
                                             QUIC_PARAM_CONN_RESUMPTION_TICKET,
                                             sslopts->session_ticket->Length,
                                             sslopts->session_ticket->Buffer
                                             )))
            {
                // Optional, just report error
                Log(LOG_ERROR, -1, "SetParam QUIC_PARAM_CONN_RESUMPTION_TICKET failed, 0x%x!\n", Status);
            }
        else
        {
            Log(TRACE_MINIMUM, -1, "SetParam QUIC_PARAM_CONN_RESUMPTION_TICKET success: len %d, bytes: %s",
                sslopts->session_ticket->Length, sslopts->session_ticket->Buffer);
            use_0rtt = TRUE;
            // Save some context for later connection start call
            net->q_ctx->server_name = host;
            net->q_ctx->server_port = port;
        }
    }

    if (QUIC_FAILED(Status = MsQuic->StreamOpen(net->q_ctx->Connection,
                                                QUIC_STREAM_OPEN_FLAG_NONE,
                                                ClientStreamCallback,
                                                net->q_ctx, &net->q_ctx->Stream)))
    {
        Log(LOG_ERROR, -1, "StreamOpen failed, 0x%x!\n", Status);
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->StreamStart(net->q_ctx->Stream, QUIC_STREAM_START_FLAG_NONE))) {
        Log(TRACE_MINIMUM, -1, "StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(net->q_ctx->Stream);
        goto exit;
    }


    Log(TRACE_MINIMUM, -1, "QUIC_new: host: %s, port: %d", host, port);
    if ( !use_0rtt )
    {
        if (QUIC_FAILED(Status = MsQuic->ConnectionStart(net->q_ctx->Connection, net->q_ctx->Configuration,
                                                         QUIC_ADDRESS_FAMILY_UNSPEC, host, port)))
        {
            Log(TRACE_MINIMUM, -1, "Start connection failed, 0x%x!\n", Status);
            free(host);
            goto exit;
        }
        free(host);
    }
    else
    {
        Log(TRACE_MINIMUM, -1, "0-RTT enabled, don't start connecion for now");
    }
    Status = 0; //success
exit:
    FUNC_EXIT_RC(Status);
    return Status;
}


int QUIC_start_0RTT_connection(QUIC_CTX *q_ctx)
{
    FUNC_ENTRY;
    QUIC_STATUS Status = 0;
    int rc = QUIC_SOCKET_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(
                        q_ctx->Connection, q_ctx->Configuration,
                        QUIC_ADDRESS_FAMILY_UNSPEC, q_ctx->server_name,
                        q_ctx->server_port)))
    {
        Log(TRACE_MINIMUM, -1, "Start 0-RTT Connection failed, 0x%x!\n", Status);
        rc = QUIC_SOCKET_ERROR;
    }
    free(q_ctx->server_name);
    q_ctx->server_name = NULL;
    q_ctx->server_port = 0;
    FUNC_EXIT_RC(rc);
    return rc;
}

int QUIC_noPendingWrites(QSOCKET socket)
{
    FUNC_ENTRY;
    FUNC_EXIT;
}

char* QUIC_getpeer(QUIC_CTX* q_ctx)
{
    FUNC_ENTRY;
    QUIC_ADDR addr;
    QUIC_STATUS Status;
    char ipstr[INET6_ADDRSTRLEN+1] = { 0 };
    // @NOTE This is actually a cache.
    char *buffer = q_ctx->peer;
    uint32_t len = sizeof(QUIC_ADDR);
    uint16_t port = 0;

    if (!q_ctx)
    {
        goto exit;
    }

    if (QUIC_FAILED(Status = MsQuic->GetParam(q_ctx->Connection,
                                     QUIC_PARAM_CONN_REMOTE_ADDRESS, &len, &addr)))
    {
        Log(LOG_ERROR, -1, "Get Peer failed: %d\n", Status);
        // we return cached value
        goto exit;
    }
    switch (addr.Ip.sa_family)
    {
        case QUIC_ADDRESS_FAMILY_INET6:
            inet_ntop(AF_INET6, &(addr.Ipv6.sin6_addr), ipstr, INET6_ADDRSTRLEN);
            port = ntohs(addr.Ipv6.sin6_port);
            break;
        default:
            inet_ntop(AF_INET, &(addr.Ipv4.sin_addr), ipstr, INET_ADDRSTRLEN);
            port = ntohs(addr.Ipv4.sin_port);
            break;
    }
    snprintf(buffer, PEER_LEN, "%s:%d", ipstr, port);
exit:
    FUNC_EXIT;
    return buffer;
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

#if defined(MSQUIC_USE_EPOLL)

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
} //QUIC_getReadySocket
#endif // MSQUIC_USE_EPOLL
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
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;


    // @TODO move to QUIC_new
    //
    if (sslopts->keyStore)
    {
        // @TODO: Support other types of credentials
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        CredConfig.CertificateFile
            = (QUIC_CERTIFICATE_FILE *)malloc(sizeof(QUIC_CERTIFICATE_FILE));

        Log(TRACE_MINIMUM, -1, "Loading certificate file: %s, CaCert: %s, \n",
            sslopts->keyStore, sslopts->trustStore);
        // Cert
        CredConfig.CertificateFile->CertificateFile = sslopts->keyStore;

        if (!sslopts->privateKey)
        {
            sslopts->privateKey = sslopts->keyStore;
        }
        CredConfig.CertificateFile->PrivateKeyFile = sslopts->privateKey;
    }

    // @NOTE, Prefer to disable validation when client cert is not provided
    //if (!sslopts->enableServerCertAuth || !sslopts->keyStore)
    // but have to follow paho current behaviour
    if (!sslopts->enableServerCertAuth)
    {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }
    else {
        //CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION;
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED;
        //CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_DEFER_CERTIFICATE_VALIDATION;
    }

    char sbuf[PATH_MAX];
    // CACert
    if (sslopts->CApath && sslopts->trustStore)
    {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;
        sprintf(sbuf, "%s/%s", sslopts->CApath, sslopts->trustStore);
        // @TODO async cert load
        CredConfig.CaCertificateFile = sbuf;
    }
    else if (sslopts->trustStore)
    {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE;

        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
        CredConfig.CaCertificateFile = sslopts->trustStore;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(q_ctx->Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &q_ctx->Configuration))) {
        Log(TRACE_MINIMUM, -1, "ConfigurationOpen failed, 0x%x!\n", Status);
        free(CredConfig.CertificateFile);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(q_ctx->Configuration, &CredConfig))) {
        Log(TRACE_MINIMUM, -1, "ConfigurationLoadCredential failed, 0x%x!\n", Status);
        free(CredConfig.CertificateFile);
        FUNC_EXIT;
        return FALSE;
    }
    free(CredConfig.CertificateFile);
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
        Log(TRACE_MINIMUM, -1, "[conn][%p] Connected\n , is resumed: %s",
            Connection, Event->CONNECTED.SessionResumed?"true":"false");
        if (q_ctx->sslkeylogfile)
        {
            dump_sslkeylogfile(q_ctx->sslkeylogfile, q_ctx->tls_secrets);
        }
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
            if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status > QUIC_STATUS_CLOSE_NOTIFY)
            {
                Log(LOG_ERROR, -1, "TLS alert: %s", ERR_reason_error_string(Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status-QUIC_STATUS_CLOSE_NOTIFY));
            }
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
            free(q_ctx->recv_buf);
            q_ctx->recv_buf = NULL;
            pthread_mutex_unlock(&q_ctx->mutex);
            // App already closed the eventfd. We only need to remove socket from the list
            Socket_close(q_ctx->Socket);
            MsQuic->ConfigurationClose(q_ctx->Configuration);
            // @NOTE never call RegistrationClose from here
            pthread_mutex_destroy(&q_ctx->mutex);
            free(q_ctx);
            goto exit;
        } else {
            q_ctx->shutdown_state = SHUTDOWN_STATE_STACK;
        }

        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        Log(TRACE_MINIMUM, -1, "[conn][%p] Resumption ticket received (%u bytes)",
            Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        if (q_ctx->nst)
        {
            QUIC_BUFFER *ticket = q_ctx->nst;
            if (ticket->Buffer)
            {
                free(ticket->Buffer);
                ticket->Buffer = NULL;
            }
            ticket->Buffer = malloc(Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
            if (!ticket->Buffer)
            {
                Log(LOG_ERROR, -1, "Failed to allocate memory for resumption ticket buffer");
                break;
            }
            ticket->Length = Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength;
            memcpy(ticket->Buffer, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket,
                   Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
            Log(TRACE_MINIMUM, -1, "Resumption ticket: updated len: %d ",
                Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
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
        if (Event->SEND_COMPLETE.Canceled)
        {
            Log(LOG_ERROR, -1, "[strm][%p] Send Canceled\n", Stream);
        }
        free(sendbuff->Buffer);
        free(sendbuff);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        if (!Event->RECEIVE.BufferCount)
        {
            ret = QUIC_STATUS_SUCCESS;
            break;
        }
        assert(q_ctx->recv_buf == NULL);
        q_ctx->recv_buf_size = Event->RECEIVE.TotalBufferLength;
        size_t len = 0;
        size_t offset = 0;
        q_ctx->recv_buf = malloc(q_ctx->recv_buf_size);

        if (!q_ctx->recv_buf)
        {
            Log(LOG_ERROR, -1, "malloc failed for %d bytes", q_ctx->recv_buf_size);
            // Return error to MsQuic for stream shutdown
            ret = QUIC_STATUS_OUT_OF_MEMORY;
            break;
        }

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
            Log(LOG_ERROR, -1, "write eventfd: %d for %d, error: %s", q_ctx->Socket, QUIC_STREAM_EVENT_RECEIVE, strerror(errno));
        }
        ret = QUIC_STATUS_PENDING;
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer aborted its send direction of the stream.
        //
        Log(TRACE_MINIMUM, -1, "[strm][%p] Peer aborted\n", Stream);
        // We abort our send direction as well, but with no error.
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        Log(TRACE_MINIMUM, -1, "[strm][%p] Peer shut down gracefully\n", Stream);
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

        // Single stream MODE
        q_ctx->is_closed = TRUE;

        if (q_ctx->shutdown_state != SHUTDOWN_STATE_APP)
        {
            if (-1 == write(q_ctx->Socket, &(uint64_t){1}, sizeof(uint64_t)))
            {
                // we only write to the eventfd if the shutdown is not initiated by the app
                // because if app closed it, the fd indexing is gone.
                Log(LOG_ERROR, -1, "write eventfd: %d for %d, error: %s", q_ctx->Socket, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE, strerror(errno));
            }
        }
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&q_ctx->mutex);
    FUNC_EXIT_RC(ret);
    return ret;
}

/*
** Re-enable receive event.
** @note, Caller should ensure thread safe
 */
void maybe_recv_complete(QUIC_CTX *q_ctx)
{
    if(!q_ctx)
    {
        return;
    }
    assert(q_ctx->recv_buf_offset <= q_ctx->recv_buf_size);
    if(q_ctx->recv_buf_offset == q_ctx->recv_buf_size)
    {
        Log(TRACE_MINIMUM, -1, "receving complete %d for %p",
            q_ctx->recv_buf_size, q_ctx->Stream);
        uint32_t completed = q_ctx->recv_buf_size;
        free(q_ctx->recv_buf);
        q_ctx->recv_buf_offset = 0;
        q_ctx->recv_buf = NULL;
        q_ctx->recv_buf_size = 0;
        MsQuic->StreamReceiveComplete(q_ctx->Stream, completed);
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
#if defined(MSQUIC_USE_EPOLL)
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = newSd;
    if (-1 == epoll_ctl(epollfd_read, EPOLL_CTL_ADD, newSd, &ev)) {
        Log(LOG_FATAL, -1, "epoll_ctl error: %s\n", strerror(errno));
    }
    Log(TRACE_MINIMUM, -1, "epoll_ctl add socket success\n");
#else
    Socket_addSocket(newSd);
#endif
exit:
	Thread_unlock_mutex(socket_mutex);
	FUNC_EXIT_RC(rc);
	return rc;
}