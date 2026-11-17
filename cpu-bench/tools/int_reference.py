"""정수 모듈 작업 단위(digest)의 독립 구현 (Python 표준 라이브러리만 사용).

C++ 구현과 같은 결과가 나오는지 교차 확인하는 용도다.
  python tools/int_reference.py 1      -> seed 1의 digest 출력
"""
import hashlib
import struct
import sys
import zlib

M = (1 << 64) - 1
BUF_BYTES = 65536
SORT_COUNT = 8192
TEXT_WORDS = 4096


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & M
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
    return state, z ^ (z >> 31)


def make_input(seed):
    state = seed & M
    words = []
    for _ in range(BUF_BYTES // 8):
        state, v = splitmix64(state)
        words.append(v)
    buf = struct.pack("<%dQ" % len(words), *words)
    u32 = struct.unpack("<%dI" % TEXT_WORDS, buf[: TEXT_WORDS * 4])
    text = "".join("%d;" % (v % 100000) for v in u32)
    return buf, text


def run_unit(buf, text):
    crc = zlib.crc32(buf) & 0xFFFFFFFF
    h = struct.unpack("<Q", hashlib.sha256(buf).digest()[:8])[0]
    arr = sorted(struct.unpack("<%dI" % SORT_COUNT, buf[: SORT_COUNT * 4]))
    s = zlib.crc32(struct.pack("<%dI" % SORT_COUNT, *arr)) & 0xFFFFFFFF
    p = sum(int(t) for t in text.split(";") if t)
    return (h ^ ((crc << 32) | s) ^ (p * 0x9E3779B97F4A7C15)) & M


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    print("0x%016x" % run_unit(*make_input(seed)))
