import math, random, time, sys
from check import sizes

def gamma_of(A):
    A=sorted(set(A))
    if len(A)<2: return float('-inf')
    n,ns,nd=sizes(A)
    if ns<=n or nd<=n: return float('-inf')
    return math.log(ns/n)/math.log(nd/n)

def local_search(start, iters, L, seed=0, verbose=False):
    random.seed(seed)
    cur=set(start); curG=gamma_of(cur)
    best=(curG,set(cur))
    for it in range(iters):
        cand=set(cur)
        op=random.random()
        if op<0.45: cand.add(random.randrange(L+1))
        elif op<0.85 and len(cand)>3: cand.remove(random.choice(sorted(cand)))
        else:
            x=random.choice(sorted(cand)); cand.discard(x); cand.add(random.randrange(L+1))
        g=gamma_of(cand)
        if g>=curG:
            cur=cand; curG=g
            if g>best[0]:
                best=(g,set(cur))
                if verbose and it%2000==0:
                    print(f"  it{it} G={g:.5f} n={len(cur)}", flush=True)
    return best

core=[0,1,2,4,5,10,13,14,15,16,18,19]
if __name__=="__main__":
    L=int(sys.argv[1]) if len(sys.argv)>1 else 60
    its=int(sys.argv[2]) if len(sys.argv)>2 else 30000
    overall=(0,set(core))
    t=time.time()
    for s in range(25):
        start=list(core)+[random.Random(s).randrange(L+1) for _ in range(random.Random(s).randint(0,15))]
        g,A=local_search(start, its, L, seed=s, verbose=True)
        if g>overall[0]:
            overall=(g,A)
            print(f"seed{s}: G={g:.5f} n={len(A)}", flush=True)
    print("TIME %.1f"%(time.time()-t))
    print("BEST", overall[0], "n", len(overall[1]))
    print(sorted(overall[1]))
