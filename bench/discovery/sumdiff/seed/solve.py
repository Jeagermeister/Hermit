# Print a finite set A of integers, separated by spaces.
# Score: Gamma(A) = log(|A+A| / |A|) / log(|A-A| / |A|). Higher is better.
# An interval scores exactly 1.0.

A = range(100)
print(" ".join(str(a) for a in A))
