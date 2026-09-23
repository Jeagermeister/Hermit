import math
from check import sizes, MAX_SPAN

def gam(A):
    A=sorted(set(A))
    if len(A)<2: return float('-inf'),0,0,0
    n,ns,nd=sizes(A)
    g=math.log(ns/n)/math.log(nd/n) if (nd>n and ns>n) else float('-inf')
    return g,n,ns,nd

def test(name,A):
    A=list(A)
    if A and A[-1]-A[0]>MAX_SPAN:
        print(f"{name}: span {A[-1]-A[0]} too big"); return
    g,n,ns,nd=gam(A)
    print(f"{name:26s} n={n:6d} |A+A|={ns:9d} |A-A|={nd:9d} S/D={ns/nd:.4f} Gamma={g:.5f}")

for n in [50,200,500,1000,1024]:
    A=[i*i for i in range(n)]
    test(f"squares n{n}",A)
for n in [100,300,500,700,900]:
    A=[i**3 for i in range(n)]
    test(f"cubes n{n}",A)
for n in [300,500,800,1200]:
    A=[i**4 for i in range(n)]
    test(f"4th pow n{n}",A)
# squares scaled by 0? already. squares minus min shift doesn't matter.
# convex: i^2 but only even i (subsample)
test("squares even n500",[ (2*i)*(2*i) for i in range(500)])
