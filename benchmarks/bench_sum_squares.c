// Benchmark 1: Sum of squares 1..N (CPU-bound integer math)
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int64_t n = 10000000;
    int64_t sum = 0;
    for (int64_t i = 1; i <= n; i++) {
        sum += i * i;
    }
    printf("%lld\n", (long long)sum);
    return 0;
}
