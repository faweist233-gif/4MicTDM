#pragma once
#include <stddef.h>
#include <stdint.h>

// 启动网络发送任务 (TCP client -> SERVER_IP:SERVER_PORT)。
void net_stream_init(void);

// 把原始交织音频写入发送缓冲; 缓冲满时丢弃 (返回实际写入字节数)。
size_t net_stream_write(const void *data, size_t len);
