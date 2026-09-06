#!/usr/bin/env python3
"""调试用：Python 移植 LZO1X + 地图包解码（对照 OpenRA LZOCompression 语义）。
成功输出 map_cells 信息；失败打印 ip 轨迹。"""
import base64
import re
import sys

def lzo1x(inp: bytes, out_cap: int) -> bytes:
    ip = 0
    op = bytearray()
    t = 0
    m_pos = 0

    def need(n):
        return ip + n <= len(inp)

    if not need(1):
        raise ValueError('no input')
    if inp[ip] > 17:
        t = inp[ip] - 17
        ip += 1
        if t < 4:
            # match_next: copy t from input, then t = *ip++
            if not need(t + 1):
                raise ValueError('match_next underrun')
            op += inp[ip:ip + t]
            ip += t
            t = inp[ip]
            ip += 1
            gt_first = False
        else:
            if not need(t):
                raise ValueError('first literals underrun')
            op += inp[ip:ip + t]
            ip += t
            gt_first = True
    else:
        gt_first = False

    while True:
        if gt_first:
            gt_first = False
        else:
            if not need(1):
                raise ValueError('opcode underrun')
            t = inp[ip]
            ip += 1
            if t >= 16:
                pass  # goto match
            else:
                # 字面量段
                if t == 0:
                    while need(1) and inp[ip] == 0:
                        t += 255
                        ip += 1
                    if not need(1):
                        raise ValueError('lit expand underrun')
                    t += 15 + inp[ip]
                    ip += 1
                if not need(t + 3):
                    raise ValueError('literals underrun')
                op += inp[ip:ip + t + 3]
                ip += t + 3
                # first_literal_run
                if not need(1):
                    raise ValueError('flr opcode underrun')
                t = inp[ip]
                ip += 1
                if t < 16:
                    if not need(1):
                        raise ValueError('flr offset underrun')
                    m_pos = len(op) - (1 + 0x800) - (t >> 2) - (inp[ip] << 2)
                    ip += 1
                    if m_pos < 0 or m_pos >= len(op):
                        raise ValueError(f'flr m_pos out: {m_pos}')
                    for _ in range(3):
                        op.append(op[m_pos])
                        m_pos += 1
                    # match_done
                    t = inp[ip - 2] & 3
                    if t == 0:
                        continue
                    # match_next from input
                    if not need(t + 1):
                        raise ValueError('flr tail underrun')
                    op += inp[ip:ip + t]
                    ip += t
                    t = inp[ip]
                    ip += 1
                    # goto match（内层循环）
        # match 内层
        while True:
            if t >= 64:
                if not need(1):
                    raise ValueError('m1 underrun')
                m_pos = len(op) - 1 - ((t >> 2) & 7) - (inp[ip] << 3)
                ip += 1
                t = (t >> 5) - 1
                if m_pos < 0 or m_pos >= len(op):
                    raise ValueError(f'm1 m_pos out: {m_pos}')
                for _ in range(t + 2):
                    op.append(op[m_pos]); m_pos += 1
            elif t >= 32:
                t &= 31
                if t == 0:
                    while need(1) and inp[ip] == 0:
                        t += 255
                        ip += 1
                    if not need(1):
                        raise ValueError('m3 expand underrun')
                    t += 31 + inp[ip]
                    ip += 1
                if not need(2):
                    raise ValueError('m3 off underrun')
                m_pos = len(op) - 1 - ((inp[ip] | (inp[ip + 1] << 8)) >> 2)
                ip += 2
                if m_pos < 0 or m_pos >= len(op):
                    raise ValueError(f'm3 m_pos out: {m_pos}')
                for _ in range(t + 2):
                    op.append(op[m_pos]); m_pos += 1
            elif t >= 16:
                if not need(2):
                    raise ValueError('m2 underrun')
                m_pos = len(op) - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while need(1) and inp[ip] == 0:
                        t += 255
                        ip += 1
                    if not need(1):
                        raise ValueError('m2 expand underrun')
                    t += 7 + inp[ip]
                    ip += 1
                m_pos -= (inp[ip] | (inp[ip + 1] << 8)) >> 2
                ip += 2
                if m_pos == len(op):
                    return bytes(op)  # eof
                m_pos -= 0x4000
                if m_pos < 0 or m_pos >= len(op):
                    raise ValueError(f'm2 m_pos out: {m_pos}')
                for _ in range(t + 2):
                    op.append(op[m_pos]); m_pos += 1
            else:
                if not need(1):
                    raise ValueError('short off underrun')
                m_pos = len(op) - 1 - (t >> 2) - (inp[ip] << 2)
                ip += 1
                if m_pos < 0 or m_pos >= len(op):
                    raise ValueError(f'short m_pos out: {m_pos}')
                for _ in range(2):
                    op.append(op[m_pos]); m_pos += 1
            # match_done
            t = inp[ip - 2] & 3
            if t == 0:
                break
            if not need(t + 1):
                raise ValueError('tail underrun')
            op += inp[ip:ip + t]
            ip += t
            t = inp[ip]
            ip += 1
            # 继续 match 内层
    return bytes(op)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else r'I:\ai\RA2R\Yuri\starcity.yrm'
    data = open(path, 'rb').read().decode('latin1')
    m = re.search(r'\[IsoMapPack5\](.*?)\[', data, re.S)
    vals = [l.split('=', 1)[1].strip() for l in m.group(1).splitlines() if '=' in l and not l.strip().startswith(';')]
    print('lines:', len(vals))
    blob = ''.join(vals)
    try:
        raw = base64.b64decode(blob)
    except Exception as e:
        raw = base64.b64decode(blob + '=' * ((4 - len(blob) % 4) % 4))
    print('decoded:', len(raw))
    mm = re.search(r'\[Map\](.*?)\[', data, re.S)
    size_line = [l for l in mm.group(1).splitlines() if l.strip().startswith('Size=')][0]
    print(size_line.strip())
    l, t_, r, b = [int(x) for x in size_line.split('=', 1)[1].split(',')]
    X = r - l + 1
    Y = b - t_ + 1
    cells_total = (X * 2 - 1) * Y
    print('expect cells:', cells_total, 'bytes:', cells_total * 11 + 4)
    pos = 0
    out = bytearray()
    chunks = 0
    while pos + 4 <= len(raw):
        comp = raw[pos] | (raw[pos + 1] << 8)
        uncomp = raw[pos + 2] | (raw[pos + 3] << 8)
        pos += 4
        if comp == 0 or uncomp == 0:
            print('terminator at', pos)
            break
        if pos + comp > len(raw):
            print('chunk overrun at', pos)
            break
        if comp == uncomp:
            out += raw[pos:pos + comp]
        else:
            out += lzo1x(raw[pos:pos + comp], uncomp)
        pos += comp
        chunks += 1
        if chunks <= 2:
            print(f'chunk {chunks}: comp={comp} uncomp={uncomp} total_out={len(out)}')
    print('chunks:', chunks, 'total out:', len(out))
    n_cells = len(out) // 11
    print('cells decoded:', n_cells, 'of', cells_total)
    # 抽查前 3 个单元格
    import struct
    for i in range(min(3, n_cells)):
        c = out[i * 11:(i + 1) * 11]
        rx, ry, tile = struct.unpack('<HHH', c[:6])
        sub, z, e2 = c[8], c[9], c[10]
        print(f'cell {i}: rx={rx} ry={ry} tile={tile} sub={sub} z={z} e2={e2}')


if __name__ == '__main__':
    main()
