# MiniReactor Phase 1

一个单线程、基于 Linux `epoll` 的最小 Reactor TCP 框架。实现了监听、连接接入、读写事件分发、输入/输出缓冲与 Echo 示例；`MyMiniReactor/` 未被使用或修改。

在 Linux 下构建并运行：

```bash
cmake -S . -B build
cmake --build build
./build/echo_server 8080
```

另一个终端可用 `nc 127.0.0.1 8080` 连接，发送的内容会被原样回显。运行 `ctest --test-dir build --output-on-failure` 可执行 Buffer 单元测试。
