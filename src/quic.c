#include "quic.h"
#include <msquic.h>

const QUIC_API_TABLE* MsQuic;

HQUIC Registration;
//
// The QUIC handle to the configuration object. This object abstracts the
// connection configuration. This includes TLS configuration and any other
// QUIC layer settings.
//
HQUIC Configuration;

BOOLEAN ClientLoadConfiguration (BOOLEAN Unsecure);


void QUIC_outInitialize(void)
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    //
    // Open a handle to the library and get the API function table.
    //
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("MsQuicOpen2 failed, 0x%x!\n", Status);
        goto Error;
    }

    if (!ClientLoadConfiguration(FALSE)) {
        goto Error;
    }
    return;
Error:
    QUIC_outTerminate();
}

void QUIC_outTerminate(void)
{
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

SOCKET QUIC_getReadySocket(int more_work, int timeout, mutex_type mutex, int* rc)
{

}

int QUIC_getch(SOCKET socket, char* c)
{

}

char *QUIC_getdata(SOCKET socket, size_t bytes, size_t* actual_len, int* rc)
{

}

int QUIC_putdatas(SOCKET socket, char* buf0, size_t buf0len, PacketBuffers bufs)
{
    QUIC_STATUS Status;
    HQUIC Stream = socket;
    uint8_t* SendBufferRaw = buf0;
    QUIC_BUFFER* SendBuffer;
    size_t SendBufferLength = buf0len;

    SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + SendBufferLength);
    if (SendBufferRaw == NULL) {
        printf("SendBuffer allocation failed!\n");
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
    SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
    SendBuffer->Length = SendBufferLength;


    if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_FIN, SendBuffer))) {
        printf("StreamSend failed, 0x%x!\n", Status);
    }



Error:
    free(SendBufferRaw);
    // @todo check status
    return Status;
}
int QUIC_close(SOCKET socket)
{

}

#if defined(__GNUC__) && defined(__linux__)
/* able to use GNU's getaddrinfo_a to make timeouts possible */
int QUIC_new(const char* addr, size_t addr_len, int port, SOCKET* socket, long timeout)
{

}
#else
int QUIC_new(const char* addr, size_t addr_len, int port, SOCKET* socket)
{

}

#endif

int QUIC_noPendingWrites(SOCKET socket)
{

}
char* QUIC_getpeer(SOCKET sock)
{

}

void QUIC_addPendingWrite(SOCKET socket)
{

}
void QUIC_clearPendingWrite(SOCKET socket)
{

}

static QUIC_writeContinue* writecontinue = NULL;
void QUIC_setWriteContinueCallback(QUIC_writeContinue* mywritecontinue)
{
  writecontinue = mywritecontinue;
}

static QUIC_writeComplete* writecomplete = NULL;
void QUIC_setWriteCompleteCallback(QUIC_writeComplete* mywritecomplete)
{
  writecomplete = mywritecomplete;
}

static QUIC_writeAvailable* writeavailable = NULL;
void QUIC_setWriteAvailableCallback(QUIC_writeAvailable* mywriteavailable)
{
  writeavailable = mywriteavailable;
}


/*
** Internals
*/
BOOLEAN
ClientLoadConfiguration(
    BOOLEAN Unsecure
    )
{
    QUIC_SETTINGS Settings = {0};
    uint64_t IdleTimeoutMs = 1000;
    QUIC_BUFFER Alpn = { sizeof("mqtt") - 1, (uint8_t*)"mqtt" };
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

    return TRUE;
}
