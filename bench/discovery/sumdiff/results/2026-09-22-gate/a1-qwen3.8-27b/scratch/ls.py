import math, random, time

def sizes(a):
    lo = a[0]; shifted=[x-lo for x in a]; span=shifted[-1]
    mask=0; rmask=0
    for x in shifted:
        mask |= 1<<x; rmask |= 1<<(span-x)
    sums=0; diffs=0
    for x in shifted:
        sums |= mask<<x; diffs |= rmask<<x
    return len(a), sums.bit_count(), diffs.bit_count()

def gamma_of(A):
    A=sorted(set(A))
    if len(A)<2: return float('-inf'),0,0,0
    n,ns,nd=sizes(A)
    if ns<=n or nd<=n: return float('-inf'),n,ns,nd
    return math.log(ns/n)/math.log(nd/n), n,ns,nd

def local_search(start, iters, L, temp=0.0, seed=0):
    random.seed(seed)
    cur=set(start); curG,_,_,_=gamma_of(cur)
    best=(curG, set(cur))
    for it in range(iters):
        cand=set(cur)
        op=random.random()
        if op<0.4:  # add random
            cand.add(random.randrange(L+1))
        elif op<0.8: # remove random
            if len(cand)>3:
                cand.remove(random.choice(sorted(cand)))
        else: # shift
            x=random.choice(sorted(cand)); y=random.randrange(L+1); 
            cand.discard(x); cand.add(y)
        g,_,_,_=gamma_of(cand)
        if g>=curG:
            cur=cand; curG=g
            if g>best[0]: best=(g,set(cur))
    return best

if __name__=="__main__":
    L=40
    overall=(0,None)
    t=time.time()
    for s in range(40):
        start=set(random.Random(s).sample(range(L+1), random.Random(s).randint(6,20)))
        g,A=local_search(start, 8000, L, seed=s)
        if g>overall[0]:
            overall=(g,A)
            print(f"iter seed{s}: Gamma={g:.5f} n={len(A)} A={sorted(A)}", flush=True)
    print("TIME", time.time()-t)
    g,A=overall
    print("BEST", g, sorted(A))
