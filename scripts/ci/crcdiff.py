r"""crcdiff.py A.log B.log - O137: compare two -logicCrcLog files frame by frame.
Prints the frame counts, the headers, and the first frame whose CRC differs (or that only one log has). Exit 0 = same."""
import sys
def load(p):
    head, rows = [], []
    for l in open(p, errors="replace"):
        l = l.strip()
        if not l: continue
        if l.startswith("#"): head.append(l); continue
        f, c = l.split(); rows.append((int(f), c))
    return head, rows
(ha, a), (hb, b) = load(sys.argv[1]), load(sys.argv[2])
print("A", len(a), "frames", ha); print("B", len(b), "frames", hb)
for i, (x, y) in enumerate(zip(a, b)):
    if x != y:
        print("FIRST DIFF at line %d: A frame %d %s / B frame %d %s" % (i, x[0], x[1], y[0], y[1]))
        n = sum(1 for p, q in zip(a, b) if p != q); print("differing lines", n, "of", min(len(a), len(b))); sys.exit(1)
if len(a) != len(b):
    print("SAME on the common %d frames, length differs" % min(len(a), len(b))); sys.exit(1)
print("SAME: all %d frames" % len(a)); sys.exit(0)
