import math, random, itertools
from check import sizes

def gam(A):
    A=sorted(set(A))
    if len(A)<2: return float('-inf'),0,0,0
    n,ns,nd=sizes(A)
    g=math.log(ns/n)/math.log(nd/n) if (nd>n and ns>n) else float('-inf')
    return g,n,ns,nd

best=(0,None)
# A = [0,m] minus holes (subset of interior)
for m in range(6, 60):
    # try single hole at each position
    for h in range(1, m):
        A=[x for x in range(m+1) if x!=h]
        g,n,ns,nd=gam(A)
        if g>best[0]: best=(g,(m,h,A[:3],A[-3:],len(A)))
print("AP-minus-1hole best:", best[0], best[1][:2], "m,h")
g,n,ns,nd = gam([x for x in range(best[1][0]+1) if x!=best[1][1]])
print("  n",n,"S",ns,"D",nd,"R",ns/nd)

# two holes
best2=(0,None)
for m in range(6,50):
    for h1 in range(1,m):
        for h2 in range(h1+1,m):
            A=[x for x in range(m+1) if x not in (h1,h2)]
            g,n,ns,nd=gam(A)
            if g>best2[0]: best2=(g,(m,h1,h2,len(A),ns,nd))
print("AP-minus-2holes best:", best2[0], best2[1])

# random holes, several
best3=(0,None)
random.seed(0)
for m in range(8,80):
    for trial in range(400):
        k=random.randint(1, max(1,m//4))
        holes=set(random.sample(range(1,m), k))
        A=[x for x in range(m+1) if x not in holes]
        g,n,ns,nd=gam(A)
        if g>best3[0]: best3=(g,(m,sorted(holes),len(A),ns,nd))
print("AP-minus-randomholes best:", best3[0], best3[1])
