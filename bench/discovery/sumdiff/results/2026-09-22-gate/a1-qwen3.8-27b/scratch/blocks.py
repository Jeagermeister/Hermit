import math
from check import sizes

def gam(A):
    A=sorted(set(A))
    if len(A)<2: return float('-inf'),0,0,0
    n,ns,nd=sizes(A)
    return (math.log(ns/n)/math.log(nd/n) if nd>n and ns>n else float('-inf')), n,ns,nd

A=[0,1,2,4,5,10,13,14,15,16,18,19]
print("core", A, gam(A))

# Two blocks: [0,a] and [b, b+c]. sweep.
def two(a,b,c):
    return list(range(0,a+1))+list(range(b,b+c+1))
best=(0,None)
for a in range(0,30):
    for b in range(1,120):
        for c in range(0,30):
            A=two(a,b,c)
            if A[-1]>400: continue
            g,n,ns,nd=gam(A)
            if g>best[0]: best=(g,A)
print("two-block best", best[0], best[1], "n",len(best[1]))

# Three blocks [0,a],[b,b+c],[d,d+e]
best3=(0,None)
for a in range(0,12):
    for b in range(1,40):
        for c in range(0,12):
            for d in range(b+c+1, b+c+30):
                for e in range(0,12):
                    A=list(range(0,a+1))+list(range(b,b+c+1))+list(range(d,d+e+1))
                    if A[-1]>300: continue
                    g,n,ns,nd=gam(A)
                    if g>best3[0]: best3=(g,A)
print("three-block best", best3[0], best3[1], "n",len(best3[1]))
