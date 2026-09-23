import random, time
from check import sizes

def ratio_of(A):
    A = sorted(set(A))
    if len(A) < 2: return (0, 0, 0)
    n, ns, nd = sizes(A)
    return (ns/nd if nd else 0, n, ns, nd)

def local_search(L, n, iters, seed):
    random.seed(seed)
    cur = set(random.sample(range(L+1), n))
    cur_r = ratio_of(cur)[0]
    best = (cur_r, set(cur))
    for it in range(iters):
        # mutate: swap a random element in for a random element out
        cand = set(cur)
        a = random.choice(sorted(cand))
        b = random.randrange(L+1)
        cand.remove(a)
        cand.add(b)
        r = ratio_of(cand)[0]
        if r >= cur_r:  # accept (allows plateaus)
            cur = cand
            cur_r = r
            if r > best[0]:
                best = (r, set(cur))
        if it % 20000 == 0:
            print(f"  iter {it}: best={best[0]:.4f} cur={cur_r:.4f}", flush=True)
    return best

L, n = 30, 12
start = time.time()
best_r = 0
bestA = None
for s in range(8):
    r, A = local_search(L, n, 60000, seed=s)
    print(f"seed {s}: best ratio={r:.4f} A={sorted(A)}")
    if r > best_r:
        best_r = r
        bestA = A
print("time", time.time()-start)
print("OVERALL BEST:", best_r, sorted(bestA))
