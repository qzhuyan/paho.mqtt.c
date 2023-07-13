# Branch: MQTT over QUIC support

## Description

## Impl. details

### Server URI schema: 'quic://'

### New Macro 'MSQUIC' wraps mqtt over quic code

### New flag 'quic' in struct **networkHandles**
The flag indicates the current connection is quic.

### New flag 'quic' in struct **MQTTAsync_struct** for async API

### New flag 'quic' in struct **MQTTClient** for sync API

### create /dev/null backed socket for adaption in **networkHandles**
Socket is used as the index for in the linked list.
How to map socket to connection/stream?

### QUIC module: quic.c and quic.h

#### Functions

1. `QUIC_handleInit`  for `MQTTAsync_global_init`
1. `MSQUIC_initialize` for `MQTTAsync_createWithOptions`
   This is per MQTTAsync creation
1. In `WebSocket_putdatas` call QUIC_putdatas   
1. [Removed]  In `MQTTProtocol_connect` distinguish ssl 3, this should be removed
1. [Removed] In `WebSocket_putdatas` call ssl if `net->ssl` eq 3

### MQTTAsync is extened for quic

1. In `MQTTAsync_connect` check options->ssl is set.
1. struct `MQTTAsync_connectOptions` is extended with quic option
1. In `MQTTAsync_processCommand`, client->quic is set to 1
1. In `MQTTAsync_completeConnection`, call QUIC_close if `net.q_ctx` is quic
1. In `MQTTAsync_connecting` apply quic schema and default port
1. In `MQTTAsync_connecting` `WAIT_FOR_CONNACK`
1. [Removed] In `MQTTAsync_createWithOptions` Set m.ssl = 3 for QUIC ssl option parsing in MQTT

### Callback triggering

#### Connected

#### Connection Lost

1. (Async API) Call `MQTTAsync_checkDisconnect` From `MQTTAsync_checkTimeouts` OR `MQTTAsync_processCommand` -> 

   Close session and then trigger
   
   Side effects: maybe reconnect
   
1. (Async API) Call `nextOrClose` From lots of callers ->
   
   Connection is going to be closed.
    
   callers are
   
   a. handle rc = `SOCKET_ERROR` in `MQTTAsync_receiveThread` that `MQTTAsync_cycle` returns
   b. checking conn timeout in `MQTTAsync_checkTimeouts`
   c. receving MQTT DISCONNECT in `MQTTAsync_receive`
   d. handle `MQTTAsync_completeConnection` fail in `MQTTAsync_receive`
   e. handle error in `MQTTAsync_connecting`
   f. handle error in `MQTTProtocol_closeSession`
   g. "CONNECT sent but MQTTPacket_Factory has returned SOCKET_ERROR"  in `MQTTAsync_cycle`

   
1. (Sync API) CAll `MQTTClient_disconnect1` when `call_connection_lost` is true 

  ▸   MQTTClient_connectURIVersion
  ▸   MQTTClient_disconnect
  ▸   MQTTClient_disconnect5
  ▸   MQTTClient_disconnect_internal
  ▸   MQTTClient_run

### New file src/QuicCTX.h file

```
typedef struct QUIC_CTX {
    pthread_mutex_t mutex;    /* mutex lock */
	HQUIC Connection;         /* MSQUIC Connection Handle */
	HQUIC Stream;             /* MSQUIC Stream Handle */
    SOCKET Socket;            /* eventfd, index of 'socket' to 'networkhandle' */
    char* recv_buf;           /* buffer to receive data */
    uint32_t recv_buf_size;   /* size of recv_buf */
    uint32_t recv_buf_offset; /* offset of unconsumed data in recv_buf */
    int is_closed;            /* Mark if control stream is closed */
} QUIC_CTX;

```

### Unlike other transports, Send data are buffered in msquic

## New Dynamic Allocations

1. Send buffer
   Allocate when send
   Free in stream callback
   
1. Recv buffer
   Allocates buffer in stream callback, event: `RECEIVE`.
   Free in receiveComplete callback
    
1. QUIC CTX
   Allocate in `QUIC_new`.
   Dealloc in  `QUIC_close`.
   
## Locks

### Mutex

1. QUIC_CTX.mutex

Init: in `QUIC_new`
Destory: in `ConnectionCallback`
Threads: PAHO recv threads and MsQuic workthreads

## Build
```
mkdir _build
cd _build
cmake -DCMAKE_BUILD_TYPE=Debug -DPAHO_WITH_MSQUIC=TRUE -DPAHO_WITH_SSL=TRUE \
-DPAHO_BUILD_SHARED=TRUE -DPAHO_BUILD_SAMPLES=TRUE ../
cmake --build ./ && src/samples/MQTTAsync_quic_publish
```

## Limitation
1. Does not support MQTT STATIC link
1. Don't support proxy 
1. Only single stream
1. No Windows support for now
1. No sync API support

## TODO

1. Double check mutex 
1. Check all callbacks
1. TLS configuration
1. Support fallback to TCP/TLS when first attempt fail
1. Doc the functions with paho doc styles
1. Add tests for quic transport
1. Support multi streams
   Connection is one MQTTAsync
   Depends on the stream type MQTTAsync could have recv thread or send thread
1. Impl get peer
1. [DONE] Support Reconnect
1. [DONE] paho style logging
1. [DONE] fixing issue provided by Memory Allocation Tracing
1. [REJECT]Set quic flag to 2 in MQTTAsync to enable TCP/TLS fallback

## References

https://eclipse.github.io/paho.mqtt.c/MQTTAsync/html/_m_q_t_t_async_8h.html
