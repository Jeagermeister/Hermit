from itertools import combinations
from check import sizes, gamma
import math

# Brute force: subsets of {0..L}, find ones with |A+A| > |A-A|
def best(L, n, cap=200):
    bestG = -1
    bestA = None
    for comb in combinations(range(L+1), n):
        ns = sizes(list(comb))
        # compute
        a = list(comb)
        n, ns_, nd_ = ns
        if nd_ == 0: continue
        g = gamma(n, ns_, nd_)
        if g > bestG:
            bestG = g
            bestA = comb
    return bestG, bestA

for n in [3,4,5,6]:
    L = 2*n + 3
    g, A = best(L, n)
    print(f"n={n}: best Gamma={g:.4f} A={A}")
