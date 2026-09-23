import random, time
from check import sizes

def rr(A):
    A = sorted(set(A))
    if len(A) < 2: return (0,0,0)
    n, ns, nd = sizes(A)
    return (ns/nd if nd else 0, n, ns, nd)

# Two well-separated intervals: {0..a} and {b, b+1, ..., b+c}
def two_interval(a, b, c):
    return set(range(0, a+1)) | set(range(b, b+c+1))

best=(1.0,None)
for a in range(1,15):
    for gap in range(1,40):
        for c in range(0,15):
            A = two_interval(a, gap, c)
            r,n,ns,nd = rr(A)
            if r>best[0]:
                best=(r, sorted(A))
                print(f"NEW two-interval a{a} gap{gap} c{c}: ratio={r:.4f} |A+A|={ns}|A-A|={nd}")
print("two-interval best:", best[0], best[1])

# mixed: dense low block + sparse high powers-of-2
def mixed(m, K, P):
    low = set(range(m))
    high = set(P*(1<<i) for i in range(K))
    return low | high
for m in range(1,30):
    for K in range(1,12):
        P = 2*m + 1
        A = mixed(m, K, P)
        r,n,ns,nd = rr(A)
        if r>best[0]:
            best=(r, sorted(A))
            print(f"NEW mixed m{m} K{K}: ratio={r:.4f}")
print("overall best:", best[0])
