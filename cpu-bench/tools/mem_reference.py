"""메모리 모듈 checksum의 독립 구현 (Python 표준 라이브러리만 사용).

C++ 구현과 같은 결과가 나오는지 교차 확인하는 용도다. 크기는 바이트.
  python tools/mem_reference.py read 1 1048576 [threads]  -> read checksum (입력 데이터 합)
  python tools/mem_reference.py copy 1 2097152 [threads]  -> copy checksum (원본 절반의 합)
  python tools/mem_reference.py write 1 1048576 [threads] -> write checksum
  python tools/mem_reference.py latency 1 65536 [threads] -> latency checksum (체인 digest 합)
"""
import sys

M = (1 << 64) - 1
LINE = 64
DIGEST_STEPS = 4096


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & M
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
    return state, z ^ (z >> 31)


def slice_bytes(total, threads):
    return total // threads // LINE * LINE


def read_checksum(seed, total, threads=1):
    words = slice_bytes(total, threads) * threads // 8
    state, s = seed & M, 0
    for _ in range(words):
        state, v = splitmix64(state)
        s = (s + v) & M
    return s


def copy_checksum(seed, total, threads=1):
    # copy는 원본·대상이 절반씩
    return read_checksum(seed, total // 2, threads)


def write_checksum(seed, total, threads=1):
    state = (seed ^ 0x5752495445) & M
    state, lo = splitmix64(state)
    state, hi = splitmix64(state)
    words = slice_bytes(total, threads) // 8
    return ((lo + hi) * (words // 2) * threads) & M


def chain_digest(seed, nodes):
    # Sattolo 순환 순열: 노드 k 다음은 order[k]
    order = list(range(nodes))
    state = seed & M
    for i in range(nodes - 1, 0, -1):
        state, v = splitmix64(state)
        j = v % i
        order[i], order[j] = order[j], order[i]
    d, idx = 0xCBF29CE484222325, 0
    for _ in range(min(nodes, DIGEST_STEPS)):
        idx = order[idx]
        d = ((d ^ idx) * 0x100000001B3) & M
    return d


def latency_checksum(seed, total, threads=1):
    nodes = slice_bytes(total, threads) // LINE
    s = 0
    for t in range(threads):
        s = (s + chain_digest(seed ^ ((t * 0xD1B54A32D192ED03) & M), nodes)) & M
    return s


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    kind, seed, total = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    threads = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    fn = {"read": read_checksum, "copy": copy_checksum, "write": write_checksum, "latency": latency_checksum}[kind]
    print("0x%016x" % fn(seed, total, threads))
    return 0


if __name__ == "__main__":
    sys.exit(main())
