# Branch: MQTT over QUIC support

## Description

## Impl. details

### Server URI schema: 'quic://'

### New Macro 'MSQUIC' wraps mqtt over quic code

### New flag 'quic' in struct networkHandles

### New flag 'quic' in struct MQTTAsync_struct

### QUIC module: quic.c and quic.h
#### Functions
1.  `QUIC_handleInit`  for `MQTTAsync_global_init`
1.  `MSQUIC_initialize` for `MQTTAsync_createWithOptions`
    This is per MQTTAsync creation
1. [Removed]  In `MQTTProtocol_connect` distinguish ssl 3, this should be removed
1. In `WebSocket_putdatas` call QUIC_putdatas
1. [Removed] In `WebSocket_putdatas` call ssl if `net->ssl` eq 3

### MQTTAsync is extened for quic
1. [Removed] In `MQTTAsync_createWithOptions` Set m.ssl = 3 for QUIC ssl option parsing in MQTT
1. In `MQTTAsync_connect` check options->ssl is set.
1. struct `MQTTAsync_connectOptions` is extended with quic option
1. In `MQTTAsync_processCommand`, client->quic is set to 1
1. In `MQTTAsync_completeConnection`, call QUIC_close if `net.q_ctx` is quic
1. In `MQTTAsync_connecting` apply quic schema and default port
1. In `MQTTAsync_connecting` `WAIT_FOR_CONNACK`

### New file src/QuicCTX file

### Unlike other transports, Send data are buffered in msquic

## Build
```
mkdir _build
cd _build
cmake -DCMAKE_BUILD_TYPE=Debug -DPAHO_WITH_MSQUIC=TRUE  -DPAHO_WITH_SSL=TRUE -DPAHO_BUILD_SHARED=TRUE ../
cmake --build ./ && src/samples/MQTTAsync_quic_publish
```

## Limitation
1. Does not support MQTT STATIC link
2. Don't support QUIC proxy for now 


## TODO

1. Support Reconnect
1. Support fallback to TCP/TLS when first attempt fail
1. Doc the functions with paho doc styles
1. paho style logging
1. fixing issue provided by Memory Allocation Tracing
1. Add tests for quic transport

## References

https://eclipse.github.io/paho.mqtt.c/MQTTAsync/html/_m_q_t_t_async_8h.html
