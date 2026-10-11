# NexusStream — Modern C++20 RTSP / RTP / HTTP-FLV Streaming Media Server & Engine

**NexusStream** 是一个面向简历/作品集与工程实践的 **现代 C++20 高性能音视频流媒体服务器与推拉流引擎**。项目从底层实现完整的 **RTSP/1.0 推拉流协议栈**、**RFC 6184 H.264 RTP 分片打包与重组 (Single NALU / STAP-A / FU-A)**、**RFC 3640 AAC RTP 传输**、**H.264 Annex-B / AVCC 码流解析与指数哥伦布 (Exp-Golomb) SPS 解码器**、**Adobe FLV / HTTP-FLV 实时封装分发**、**首屏秒开 GOP Cache**，以及 **FFmpeg (`libavformat`/`libavcodec`) C++20 RAII 封装桥接层**。

---

## 核心特性 (Key Highlights)

1. **高性能 Reactor 网络模型 (`epoll` + `eventfd` + 非阻塞 I/O)**
   - 基于 Linux `epoll` 多路复用与 `eventfd` 跨线程唤醒机制构建的 `EventLoop` 与 `TcpServer`。
   - 零拷贝引用计数媒体帧 [`MediaFrame`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/core/media_frame.hpp)：单帧编码载荷采用 `std::shared_ptr<const std::vector<uint8_t>>`，在向成百上千个 RTSP / HTTP-FLV 订阅端扇出广播时实现 **零内存拷贝**。
   - 线程安全有界阻塞队列 [`BoundedBlockingQueue<T>`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/core/bounded_queue.hpp)：支持背压阻塞与实时直播高水位丢弃旧帧策略 (`drop_oldest_if_full`)。

2. **完备的音视频码流解析器 (Zero-Dependency Native Codec Engine)**
   - **H.264 码流引擎 ([`H264Parser`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/codec/h264_parser.hpp))**：
     - Annex-B 起始码 (`0x000001` / `0x00000001`) 状态机切分与 AVCC (4字节大端长度前缀) 双向转换。
     - 防竞争字节 (`0x00 0x00 0x03`) 脱壳与加壳 (EBSP $\leftrightarrow$ RBSP)。
     - 手写位级读取器 [`BitReader`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/codec/bit_reader.hpp) 与无符号/有符号指数哥伦布编码 (`ue(v)` / `se(v)`)，直接从 SPS NALU 解析视频宽高 (`width`/`height`)、帧率 (`fps`)、`profile_idc` 与 `level_idc`。
     - 生成并解析 ISO/IEC 14496-15 `AVCDecoderConfigurationRecord`。
   - **AAC 音频引擎 ([`AacParser`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/codec/aac_parser.hpp))**：
     - 7/9 字节 ADTS 头解析与封装。
     - ISO/IEC 14496-3 2字节 `AudioSpecificConfig` (ASC) 编码与解码。

3. **标准流媒体传输协议栈 (RTSP / RTP / RTCP / SDP / HTTP-FLV)**
   - **RFC 6184 H.264 RTP 打包与解包器 ([`H264RtpPacketizer`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/protocol/rtp_packet.hpp))**：
     - 小于等于 MTU (1400B) 的 NALU 采用 Single NAL Unit 模式；
     - 大于 MTU 的 IDR 关键帧自动切分为 **FU-A (Fragmentation Unit A, Type 28)** 分片包（精确设置 Start/End 位与 Marker 位），接收端通过状态机无损重组。
   - **RFC 3640 AAC RTP 打包与解包器 ([`AacRtpCodec`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/protocol/rtp_packet.hpp))**：支持 `AAC-hbr` 16-bit `AU-headers-length` + 13-bit 帧长 `AU-header`。
   - **RTSP/1.0 服务端与推拉流客户端 ([`RtspServer`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/media/rtsp_server.hpp))**：
     - 完整支持 **RTSP 推流 (`OPTIONS` $\rightarrow$ `ANNOUNCE` $\rightarrow$ `SETUP` $\rightarrow$ `RECORD`)** 与 **RTSP 拉流 (`OPTIONS` $\rightarrow$ `DESCRIBE` $\rightarrow$ `SETUP` $\rightarrow$ `PLAY` $\rightarrow$ `TEARDOWN`)**。
     - 支持 `$` 交织帧 (RTP over TCP Interleaved) 同连接复用信令与媒体数据。
   - **HTTP-FLV 实时流分发与监控面板 ([`HttpFlvServer`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/media/http_flv_server.hpp))**：
     - 实时将 H.264 + AAC 复用为标准 FLV 流 (`GET /live/<stream>.flv`)，可直接用 `ffplay`、`VLC` 或 `flv.js` 播放。
     - 内置实时 Web 监控大盘 (`GET /`) 与 JSON 指标接口 (`GET /api/streams`)。

4. **直播首屏秒开优化 ([`GopCache`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/media/gop_cache.hpp))**
   - 缓存最新的序列头 (`SPS/PPS`、`AudioSpecificConfig`) 以及从最近一个 H.264 IDR 关键帧开始的完整 GOP 帧序列。
   - 新观众接入 RTSP 或 HTTP-FLV 时立即下发序列头与缓存 GOP，实现 **0 ms 等待首屏关键帧秒开**。

5. **FFmpeg (`libav*`) C++20 RAII 桥接层 ([`ffmpeg_raii.hpp`](file:///home/zhangxiaoji/.gemini/antigravity/scratch/nexus-stream/include/nexus/ffmpeg/ffmpeg_raii.hpp))**
   - 使用自定义 Deleter 的 `std::unique_ptr` 封装 `AVFormatContext`、`AVCodecContext`、`AVPacket`、`AVFrame`、`AVBSFContext`，彻底杜绝资源泄漏。

---

## 目录结构 (Directory Structure)

```text
nexus-stream/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── ARCHITECTURE_AND_INTERVIEW_GUIDE.md   # 架构设计、协议字节序详解与音视频面试指南
├── include/nexus/
│   ├── core/
│   │   ├── bounded_queue.hpp                 # 线程安全有界阻塞队列
│   │   ├── ring_buffer.hpp                   # 网络字节缓冲区 (支持 Big-Endian 16/24/32位读写)
│   │   ├── logger.hpp                        # 线程安全分级日志器
│   │   └── media_frame.hpp                   # 零拷贝引用计数音视频帧与轨道元数据
│   ├── codec/
│   │   ├── bit_reader.hpp                    # 位流读写器与 Exp-Golomb (ue/se) 编解码
│   │   ├── h264_parser.hpp                   # H.264 Annex-B/AVCC 解析、EBSP脱壳、SPS解析
│   │   ├── aac_parser.hpp                    # AAC ADTS 解析封装与 AudioSpecificConfig
│   │   └── test_stream_generator.hpp         # 标准 H.264 (SPS/PPS/IDR/P) + AAC 码流合成器
│   ├── protocol/
│   │   ├── rtp_packet.hpp                    # RFC 3550 RTP/RTCP, RFC 6184 FU-A, RFC 3640 AAC
│   │   ├── sdp.hpp                           # SDP 会话描述生成与解析
│   │   ├── rtsp_message.hpp                  # RTSP/1.0 请求/响应解析与序列化
│   │   └── flv_muxer.hpp                     # Adobe FLV / HTTP-FLV 复用器与解析器
│   ├── net/
│   │   ├── socket_utils.hpp                  # RAII ScopedFd 与非阻塞 Socket 工具
│   │   ├── event_loop.hpp                    # Linux epoll + eventfd Reactor 事件循环
│   │   └── tcp_server.hpp                    # 多连接非阻塞 TCP 服务端
│   ├── media/
│   │   ├── gop_cache.hpp                     # 直播首屏秒开 GOP 缓存器
│   │   ├── media_hub.hpp                     # Pub/Sub 媒体会话中心与流统计
│   │   ├── rtsp_server.hpp                   # RTSP 推拉流服务器与客户端
│   │   └── http_flv_server.hpp               # HTTP-FLV 直播服务器与 Web 监控大盘
│   └── ffmpeg/
│       └── ffmpeg_raii.hpp                   # FFmpeg libav* C++20 RAII 封装与解复用器
├── src/                                      # 核心实现与可执行程序源码
└── tests/
    └── test_main.cpp                         # 8大模块单元与全链路集成测试套件
```

---

## 快速编译与运行 (Quick Start)

### 1. 编译项目
```bash
cmake -B build
cmake --build build -j4
```

### 2. 运行单元测试与全链路集成测试
```bash
ctest --test-dir build --output-on-failure
```

### 3. 运行端到端基准测试 (Benchmark)
```bash
./build/nexus_bench
```

### 4. 启动流媒体服务器与推拉流工具
```bash
# 终端 1：启动 RTSP (8554) + HTTP-FLV & Web Dashboard (8080) 服务器（默认自带 /live/demo 实时流）
./build/nexus_server --rtsp-port 8554 --http-port 8080

# 终端 2：使用 nexus_pusher 推送一路 1080p H.264 + AAC 直播流到 /live/stream1，并导出标准 FLV 与 H.264 文件
./build/nexus_pusher --host 127.0.0.1 --port 8554 --path /live/stream1 --frames 150 \
    --export-flv sample.flv --export-h264 sample.h264

# 终端 3：查看实时 JSON 流状态或用 ffplay/VLC 拉流
curl http://127.0.0.1:8080/api/streams
# ffplay rtsp://127.0.0.1:8554/live/demo
# ffplay http://127.0.0.1:8080/live/demo.flv
```
