// Benchmark 2: Fibonacci (recursive function calls)
#include <stdio.h>
#include <stdint.h>

int64_t fibonacci(int64_t n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int main(void) {
    printf("%lld\n", (long long)fibonacci(40));
    return 0;
}
