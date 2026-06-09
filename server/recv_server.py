#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
四声道(PCM1865 TDM) WiFi 音频接收服务器
====================================================
监听 TCP, 接收 ESP32-C3 推来的自定义帧, 解析 + 按 seq 检测丢包,
反交织成 4 通道, 写入一个 4 声道 WAV 文件。

帧格式 (小端):
  magic[4]='A','4','C','H' | ver(1) | ch(1) | bits(1) | rsv(1)
  sample_rate(4) | seq(4) | n_samples(4) | payload(交织 int16 * ch * n_samples)

用法:
  python recv_server.py --host 0.0.0.0 --port 9000 --outdir ./recordings
"""
import argparse
import os
import signal
import socket
import struct
import time
import wave

MAGIC = b'A4CH'
HDR_FMT = '<4sBBBBIII'          # magic, ver, ch, bits, rsv, sr, seq, n_samples
HDR_SIZE = struct.calcsize(HDR_FMT)   # = 20

STOP = False                    # Ctrl+C / SIGTERM 置位, 用于优雅退出


def recv_exact(sock, n):
    """阻塞收满 n 字节; 连接关闭/收到停止信号返回 None。
    sock 已设 settimeout, 超时只是「还没来」, 继续等; 借机让 Ctrl+C 投递。"""
    buf = bytearray()
    while len(buf) < n:
        if STOP:
            return None
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            continue            # 没数据但连接还在, 回头检查 STOP 后再等
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def patch_wav_sizes(wf):
    """把当前已写字节数回填进 WAV 头的 RIFF-size 与 data-size 字段, 然后回到末尾。
    这样即使进程被强杀(finally 没跑到), 文件也是个合法、可读出正确时长的 WAV。
    依赖 CPython wave 模块标准 44 字节头布局: RIFF-size@4, data-size@40。"""
    f = wf._file                        # 底层文件对象
    n = wf._datawritten                 # 已写音频字节
    end = f.tell()
    f.seek(4);  f.write(struct.pack('<I', 36 + n))   # RIFF chunk size
    f.seek(40); f.write(struct.pack('<I', n))        # data chunk size
    f.seek(end)
    f.flush()


def resync_to_magic(sock, first_byte=b''):
    """逐字节滑动直到对齐 4 字节 magic, 返回紧随其后的剩余 16 字节头。"""
    window = bytearray(first_byte)
    while True:
        b = sock.recv(1)
        if not b:
            return None
        window.extend(b)
        if len(window) > 4:
            window = window[-4:]
        if bytes(window) == MAGIC:
            rest = recv_exact(sock, HDR_SIZE - 4)
            return rest


def open_wav(outdir, sr, ch, sampwidth):
    os.makedirs(outdir, exist_ok=True)
    ts = time.strftime('%Y%m%d_%H%M%S')
    path = os.path.join(outdir, f'rec_{ts}_{sr}Hz_{ch}ch.wav')
    wf = wave.open(path, 'wb')
    wf.setnchannels(ch)
    wf.setsampwidth(sampwidth)
    wf.setframerate(sr)
    print(f'[REC] 写入 {path}')
    return wf, path


def handle_client(conn, addr, outdir):
    print(f'[CONN] 客户端 {addr} 接入')
    conn.settimeout(1.0)        # 让阻塞 recv 周期性返回, Ctrl+C 才能在 Windows 上生效
    wf = None
    path = None
    expected_seq = None
    total_frames = 0
    lost = 0
    t0 = time.time()
    try:
        while True:
            # --- 读 4 字节 magic, 不匹配则重新对齐 ---
            head = recv_exact(conn, 4)
            if head is None:
                break
            if head != MAGIC:
                rest = resync_to_magic(conn, head)
                if rest is None:
                    break
            else:
                rest = recv_exact(conn, HDR_SIZE - 4)
                if rest is None:
                    break

            (_, ver, ch, bits, _rsv, sr, seq, n_samples) = struct.unpack(HDR_FMT, MAGIC + rest)
            sampwidth = bits // 8
            payload_len = n_samples * ch * sampwidth
            payload = recv_exact(conn, payload_len)
            if payload is None:
                break

            # --- 丢包检测 ---
            if expected_seq is not None and seq != expected_seq:
                gap = (seq - expected_seq) & 0xFFFFFFFF
                lost += gap
                print(f'[LOSS] seq 跳变 {expected_seq}->{seq} (丢 {gap})')
            expected_seq = (seq + 1) & 0xFFFFFFFF

            # --- 首帧建文件 ---
            if wf is None:
                wf, path = open_wav(outdir, sr, ch, sampwidth)

            # payload 已是通道交织, wave 模块要求的就是交织格式, 直接写
            wf.writeframes(payload)
            total_frames += n_samples

            if total_frames % (sr * 5) < n_samples:   # 每约 5s 报一次 + 封口
                patch_wav_sizes(wf)                    # 周期回填头, 强杀也不丢
                dur = total_frames / sr
                print(f'[STAT] 已收 {dur:.1f}s, 丢包 {lost}, 速率 '
                      f'{total_frames/max(time.time()-t0,1e-3):.0f} samp/s')
    finally:
        if wf:
            patch_wav_sizes(wf)     # 先回填一次, 再 close (close 内部也会回填)
            fr = wf.getframerate() or 1
            wf.close()
            print(f'[DONE] {addr} 断开, 时长 {total_frames/fr:.1f}s, 总丢包 {lost}, 文件 {path}')
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description='4ch TDM 音频接收服务器')
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=9000)
    ap.add_argument('--outdir', default='./recordings')
    args = ap.parse_args()

    def _stop(signum, frame):
        global STOP
        STOP = True
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, _stop)
    try:
        signal.signal(signal.SIGTERM, _stop)   # kill / 部分 taskkill 场景
    except (ValueError, AttributeError):
        pass

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    srv.settimeout(1.0)            # accept 也可被 Ctrl+C 打断
    print(f'[LISTEN] {args.host}:{args.port}  输出目录 {args.outdir}')
    print('等待 ESP32-C3 连接... (Ctrl+C 退出)')
    try:
        while not STOP:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            handle_client(conn, addr, args.outdir)   # 单客户端串行处理
    except KeyboardInterrupt:
        print('\n退出。')
    finally:
        srv.close()


if __name__ == '__main__':
    main()
