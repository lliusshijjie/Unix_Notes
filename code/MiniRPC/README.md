# MiniRPC

MiniRPC 是建立在 MiniReactor 上的 C++17 最小 RPC 实现。第一版提供自定义
二进制协议、流式 Codec、服务注册与路由、业务线程池、同步 RpcClient、
PendingCall、RpcController 和 RpcChannel。

## 构建与测试

仅支持 Linux，要求 CMake 3.20 以上和 GCC：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

AddressSanitizer 与 ThreadSanitizer 必须使用不同构建目录：

```bash
cmake -S . -B build-asan -DMINIRPC_ENABLE_ASAN=ON
cmake --build build-asan -j2
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DMINIRPC_ENABLE_TSAN=ON
cmake --build build-tsan -j2
ctest --test-dir build-tsan --output-on-failure
```

## Calculator 示例

先启动服务端：

```bash
./build/calculator_server 8080
```

再启动客户端：

```bash
./build/calculator_client 127.0.0.1 8080
```

正常输出：

```text
10 + 20 = 30
```

第一版不提供 Protobuf、自动重连、自动重试、连接池、服务发现、TLS、
流式 RPC 或异步 Future API。
