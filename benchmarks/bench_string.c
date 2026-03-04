// Benchmark 4: String operations (runtime)
// String replace in a loop (mimics MIX runtime behavior)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *str_replace(const char *src, const char *find, const char *replace) {
    size_t find_len = strlen(find);
    size_t replace_len = strlen(replace);
    const char *pos = strstr(src, find);
    if (!pos) {
        return strdup(src);
    }
    size_t src_len = strlen(src);
    size_t new_len = src_len - find_len + replace_len;
    char *result = malloc(new_len + 1);
    size_t prefix_len = pos - src;
    memcpy(result, src, prefix_len);
    memcpy(result + prefix_len, replace, replace_len);
    memcpy(result + prefix_len + replace_len, pos + find_len,
           src_len - prefix_len - find_len + 1);
    return result;
}

int main(void) {
    char *s = strdup("hello world");
    for (int i = 0; i < 100000; i++) {
        char *t1 = str_replace(s, "world", "mix");
        free(s);
        s = t1;
        char *t2 = str_replace(s, "mix", "world");
        free(s);
        s = t2;
    }
    printf("%s\n", s);
    free(s);
    return 0;
}
