import math, random
from check import sizes

def metrics(A):
    A = sorted(set(A))
    n, ns, nd = sizes(A)
    g = math.log(ns/n)/math.log(nd/n) if nd>n and ns>n else float('nan')
    return g, n, ns, nd

A = (0,2,3,4,7,11,12,14)
g,n,ns,nd = metrics(A)
print("winner:", A, "n",n,"S",ns,"D",nd,"S/D",ns/nd,"Gamma",g)
print("  2n-1 =", 2*n-1, " S/n=%.3f D/n=%.3f"%(ns/n, nd/n))

# Show the actual sum & diff sets
s = sorted({a+b for a in A for b in A})
d = sorted({a-b for a in A for b in A})
print("  A+A=",s)
print("  A-A=",d)
print("  missing from [-14,28] sums? full span:", 2*14+1)
