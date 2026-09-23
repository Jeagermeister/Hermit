import math
from check import sizes, gamma, MAX_SPAN, MAX_ELEMENTS

def score(A):
    A = sorted(set(A))
    if len(A) < 2 or len(A) > MAX_ELEMENTS or A[-1]-A[0] > MAX_SPAN:
        return None
    n, ns, nd = sizes(A)
    return gamma(n, ns, nd), n, ns, nd

def try_(name, A):
    r = score(A)
    if r is None:
        print(f"{name}: INVALID (too big / span)")
        return
    g, n, ns, nd = r
    print(f"{name:24s} Gamma={g:.5f}  n={n:6d} |A+A|={ns:8d} |A-A|={nd:8d} "
          f"ratio=|A+A|/|A-A|={ns/nd:.4f}")

# baseline
try_("interval", list(range(100)))
try_("interval20000", list(range(20000)))

# powers of 2
for k in [21, 30, 100]:
    A = [1 << i for i in range(k)]
    if A[-1] - A[0] <= MAX_SPAN:
        try_(f"pow2_k{k}", A)
    else:
        print(f"pow2_k{k}: span too big")

# powers of 2 shifted: {2^i} plus 0
for k in [21]:
    A = [0] + [1 << i for i in range(k)]
    try_(f"pow2_0_k{k}", A)

# product set A = X*P + Y
def product(X, Y, P):
    return [x*P + y for x in X for y in Y]

# X powers of 2, Y interval
for k in [11]:
    X = [1 << i for i in range(k)]
    for m in [1, 2, 10, 100, 500]:
        P = 2*m  # need P > 2(m-1)
        A = product(X, list(range(m)), P)
        try_(f"prod X=pow2 k{k} m{m} P{P}", A)
