import random, math
from check import sizes, gamma

def ratio(A):
    A = sorted(set(A))
    if len(A) < 2: return None
    n, ns, nd = sizes(A)
    return ns/nd, n, ns, nd

best_r = 1.0
bestA = None

def report(tag, A):
    global best_r, bestA
    r = ratio(A)
    if r is None: return
    rt, n, ns, nd = r
    if rt > best_r:
        best_r = rt
        bestA = (tag, list(A), rt, n, ns, nd)
        print(f"NEW BEST  {tag:20s} ratio={rt:.5f} n={n:4d} |A+A|={ns:7d} |A-A|={nd:7d}")

random.seed(1)
# random subsets of {0..L}
for trial in range(200000):
    L = random.randint(10, 40)
    n = random.randint(6, 16)
    A = random.sample(range(L+1), n)
    r = ratio(A)
    rt, n_, ns, nd = r
    if rt > best_r:
        best_r = rt
        print(f"NEW BEST  rand L{L} n{n} ratio={rt:.5f} |A+A|={ns} |A-A|={nd} A={sorted(A)[:20]}")
        if best_r > 1.3:
            break

print("best random ratio:", best_r)
