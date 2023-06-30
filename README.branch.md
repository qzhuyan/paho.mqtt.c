# Document Title
## build
```
mkdir _build
cd _build_ 
cmake -DCMAKE_BUILD_TYPE=Debug -DPAHO_WITH_MSQUIC=TRUE  -DPAHO_WITH_SSL=TRUE -DPAHO_BUILD_SHARED=TRUE ../
cmake --build ./ && src/samples/MQTTAsync_quic_publish
```
