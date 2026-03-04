// Benchmark 3: List operations (heap allocation + iteration)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(void) {
    int64_t n = 1000000;
    int64_t *nums = malloc(n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        nums[i] = i;
    }

    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        total += nums[i];
    }
    printf("%lld\n", (long long)total);
    free(nums);
    return 0;
}
