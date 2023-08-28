# Branch: MQTT over QUIC support

## Description

1. Add MQTT over QUIC support.
   
1. Fix some coredumps from the master branch.

1. Fix some flaky tests from master branch.

For MQTT over QUIC, check: https://www.emqx.io/docs/en/v5.1/mqtt-over-quic/introduction.html

## Build
``` bash
mkdir _build
cd _build
cmake -DCMAKE_BUILD_TYPE=Debug -DPAHO_WITH_MSQUIC=TRUE -DPAHO_WITH_SSL=TRUE \
-DPAHO_BUILD_SHARED=TRUE -DPAHO_BUILD_SAMPLES=TRUE ../
cmake --build 
```

## Example Code

For Async Client Publish see: [Publish](src/samples/MQTTAsync_quic_publish.c)

For Async Client Subscribe see: [Subscribe](src/samples/MQTTAsync_quic_subscribe.c)

Or just compare with the basic ones

 ``` bash
 diff src/samples/MQTTAsync_quic_subscribe.c  src/samples/MQTTAsync_subscribe.c 
 ```

## Debug Methods 

### Build with lttng tracing.
``` bash
mkdir _build
cd _build
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DPAHO_WITH_MSQUIC=TRUE \
      -DPAHO_WITH_SSL=TRUE \
      -DPAHO_BUILD_SHARED=TRUE \
      -DPAHO_BUILD_SAMPLES=TRUE \
      -DPAHO_ENABLE_QUIC_LOGGING=TRUE \
      ../
cmake --build 
```

### Inpsect traffic with wireshark, requires 4.0.6+

1. Method 1
```
# Start wireshrak and load lua script that supports MQTT over QUIC
cd test
wireshark -X lua_script:mqtt_over_quic.lua
```

1. Method 2

Download&Install supported wireshark from here:

[Wireshark + MQTTOverQUIC](https://emqx-my.sharepoint.com/personal/william_yang_emqx_io/_layouts/15/onedrive.aspx?id=%2Fpersonal%2Fwilliam%5Fyang%5Femqx%5Fio%2FDocuments%2FWireshark%2BMQTTOverQUIC%2Fubuntu%2Dpackages&view=0
)


## Tests

New tests are added in test9000 named after `rfc9000`.

You need to start a EMQX broker with configuration written for this test env.
take a look in [test/emqx.conf](test/emqx.conf).

### Start EMQX broker. 
```sh
curl -L -o emqx.tar.gz https://github.com/emqx/emqx/releases/download/v5.1.1/emqx-5.1.1-ubuntu22.04-amd64.tar.gz
mkdir -p emqx
tar zxf emqx.tar.gz -C emqx
cat test/emqx.conf >> emqx/etc/emqx.conf
emqx/bin/emqx start
```

### Run tests against EMQX
``` sh
ctest -R test9000 
```

## Impl. details

### Server URI schema: 'quic://'

### link to [MsQuic](https://github.com/microsoft/msquic)

### New Macro 'MSQUIC' wraps mqtt over quic code

### New flag 'quic' in struct **networkHandles**
The flag indicates the current network transport in use is quic.

### New flag 'quic' in struct **MQTTAsync_struct** for async API

### New flag 'quic' in struct **MQTTClient** for sync API

### eventfd backed 'socket' for adaption in **networkHandles**
Socket is used as the index for networkhandles in a linked list.
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

Callbacks are kept as they are.

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

## What is working

1. QUIC handshake works, with verify server, verify client and mTLS, success or failure as it should.
1. pub/sub works over a single stream
1. Qos 0, 1, 2 checked
1. Connection and Stream Teardown.
1. send/recv large messages.
1. multiple concurrent clients. (QUIC clients only, Mixed TLS/QUIC clients, untested).
1. Coexists with TCP/TLS.
1. Fullstack debug loggings.
1. pre-master-key export for wireshark decryption. (set via envvar `SSLKEYLOGFILE`).
1. Github CI in Linux (ubuntu 22.04)

## Limitations

1. Does not support STATIC link
1. Only support Async Client
1. Don't support proxy 
1. Support single stream, no multistreams for now.
1. No Windows support
1. Everthing is async no sync API.

## TODOs/Known issues

1. Double check mutex usages.
   All `extern mutex_type` are in `MQTTAsyncUtils.c`
1. Doc the functions with paho doc styles
1. 0-RTT support
1. [DEFER] Maybe SendBuffer
1. [DEFER] Support multi streams
   PAHO has three threads. (send thread, recv thread and the caller thread)
   Need some model check before we extend it to have more threads. 
1. [DONE] Checked callbacks
   - onConnect
   - onConnectFailure
   - onSubscribe
   - onSubscribeFailure
   - onUnSubscribe
   - onDisconnect
   - onMessageArrived
   - onPublishSuccess
   - onPublishFailure

1. [DONE] call `QUIC_close` in MQTTClient_closeSession
1. [DONE] Impl get peer API.
1. [DONE] QUIC and none QUIC both should use poll
1. [DONE] Add tests for quic transport
1. [DONE] TLS configuration
1. [DONE] Support Reconnect
1. [DONE] paho style logging
1. [DONE] fixing issue provided by Memory Allocation Tracing
1. [DONE] RECEIVE buffer, reused SocketBuffer
1. [REJECT] Support fallback to TCP/TLS when first attempt fail
1. [REJECT] Set quic flag to 2 in MQTTAsync to enable TCP/TLS fallback


## References

https://eclipse.github.io/paho.mqtt.c/MQTTAsync/html/_m_q_t_t_async_8h.html
