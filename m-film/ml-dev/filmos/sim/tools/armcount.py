"""Count ARM instructions executed, per function, from qemu-arm's log.

    qemu-arm -d in_asm,exec,nochain -D /dev/stderr ./prog 2>&1 >/dev/null | python3 armcount.py

The camera has no profiler; this gives the instruction budget of the
same code compiled for ARMv5TE."""

import bisect
import re
import subprocess
import sys
from collections import Counter

# qemu leaves some blocks unnamed; name them from the binary's symbols
syms = []
if len(sys.argv) > 1:
    for l in subprocess.run(["arm-linux-gnueabi-nm", "-n", sys.argv[1]], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]

def name_of(pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i][1] if i >= 0 else "?"

tb_size = {}
tb_func = {}
execs = Counter()
cur_func, cur_start, cur_n = None, None, 0
insn = re.compile(r"^0x([0-9a-f]+):\s")
trace = re.compile(r"^Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/")

for line in sys.stdin:
    if line.startswith("Trace"):
        m = trace.match(line)
        if m:
            execs[int(m.group(1), 16)] += 1
        continue
    if line.startswith("IN:"):
        cur_func = line[3:].strip() or "?"
        cur_start, cur_n = None, 0
        continue
    m = insn.match(line)
    if m and cur_func is not None and cur_start is None:
        cur_start = int(m.group(1), 16)
        continue
    if line.startswith("OBJD-T:") and cur_start is not None:
        # no disassembler in this qemu: raw bytes, 4 per ARM instruction
        cur_n += len(line[7:].strip()) // 8
        tb_size[cur_start] = cur_n
        tb_func[cur_start] = cur_func

total = 0
per = Counter()
for pc, n in execs.items():
    k = n * tb_size.get(pc, 0)
    total += k
    f = tb_func.get(pc, "?")
    if f == "?" and syms:
        f = name_of(pc)
    per[f] += k
print(f"total {total:,} ARM instructions")
if len(sys.argv) > 2 and sys.argv[2] == "--bylines":
    # every block attributed to the source line of its first instruction
    pcs = list(execs)
    res = subprocess.run(["arm-linux-gnueabi-addr2line", "-e", sys.argv[1], "-s"], input="\n".join(hex(pc) for pc in pcs),
                         capture_output=True, text=True).stdout.splitlines()
    by = Counter()
    for pc, where in zip(pcs, res):
        by[where.split(" ")[0]] += execs[pc] * tb_size.get(pc, 0)
    for w, k in by.most_common(60):
        print(f"{k:14,}  {100.0 * k / total:5.1f}%  {w}")
    sys.exit(0)
if len(sys.argv) > 2 and sys.argv[2] == "--lines":
    # the hottest blocks, with their source lines
    hot = sorted(((n * tb_size.get(pc, 0), pc) for pc, n in execs.items()), reverse=True)[:40]
    pcs = [hex(pc) for _, pc in hot]
    lines = subprocess.run(["arm-linux-gnueabi-addr2line", "-e", sys.argv[1], "-f", "-s", "-i"] + pcs,
                           capture_output=True, text=True).stdout.splitlines()
    # -i may print several frames per address; keep the first two lines of each
    out, i = [], 0
    res = subprocess.run(["arm-linux-gnueabi-addr2line", "-e", sys.argv[1], "-s"] + pcs, capture_output=True, text=True).stdout.splitlines()
    for (k, pc), where in zip(hot, res):
        print(f"{k:14,}  {100.0 * k / total:5.1f}%  {hex(pc)}  {tb_func.get(pc, name_of(pc) if syms else '?')}  {where}")
    sys.exit(0)
for f, k in per.most_common(25):
    try:
        print(f"{k:14,}  {100.0 * k / total:5.1f}%  {f}")
    except BrokenPipeError:
        break
