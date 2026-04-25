#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int n = 1000000;
    long *items = malloc(sizeof(long) * n);
    int top = 0;
    for (int i = 0; i < n; i++) items[top++] = i;
    long sum = 0;
    while (top > 0) sum += items[--top];
    free(items);
    printf("%ld\n", sum);
    return 0;
}
