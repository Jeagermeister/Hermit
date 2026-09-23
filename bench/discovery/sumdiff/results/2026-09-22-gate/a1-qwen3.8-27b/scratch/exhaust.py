import math
from check import sizes

def full_search(L):
    best_r = 0; bestA=None
    best_g = 0; bestG=None
    from itertools import combinations
    for n in range(2, L+1):
        for comb in combinations(range(L+1), n):
            a = list(comb)
            nn, ns, nd = sizes(a)
            r = ns/nd
            if r > best_r:
                best_r = r; bestA = comb
            g = math.log(ns/nn)/math.log(nd/nn)
            if g > best_g:
                best_g = g; bestG = comb
    return best_r, bestA, best_g, bestG

for L in [10, 12, 14]:
    r, A, g, G = full_search(L)
    print(f"L={L}: best ratio={r:.5f} A={A}")
    print(f"      best Gamma={g:.5f} A={G}  (sizes n={len(G)} |A+A|? )")
