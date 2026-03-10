#ifndef TEST_USE_C_H
#define TEST_USE_C_H

// Constants
#define ANSWER 42
#define HEX_MASK 0xFF00
#define PI_APPROX 3.14

// Enum
enum Direction {
    DIR_NORTH = 0,
    DIR_EAST,
    DIR_SOUTH,
    DIR_WEST
};

typedef enum {
    COLOR_RED = 0x01,
    COLOR_GREEN = 0x02,
    COLOR_BLUE = 0x04
} Color;

// A function implemented in the companion .c file
int test_add(int a, int b);
int test_multiply(int a, int b);

#endif
