/* cc -std=c11 -Wall -Wextra -Werror firmware/host_test/lcd35b_geometry_test.c -o /tmp/lcd35b-geometry && /tmp/lcd35b-geometry */
#include <assert.h>
#include <stdio.h>
#ifndef TANK_LCD35B
#define TANK_LCD35B
#endif
#include "../../common/tank.h"
#include "../main/lcd35b_geometry.h"
int main(void) {
    int x, y;
    assert(TANK_W * 320 == TANK_H * 480); /* uniform scaling, no stretch */
    assert(lcd35b_point(0, 479, &x, &y) && x == 0 && y == 0);
    assert(lcd35b_point(319, 479, &x, &y) && x == 0 && y == TANK_H-1);
    assert(lcd35b_point(0, 0, &x, &y) && x == TANK_W-1 && y == 0);
    assert(lcd35b_point(319, 0, &x, &y) && x == TANK_W-1 && y == TANK_H-1);
    /* Full frame covers all four edges; no dead band at a control or gesture
       boundary. Every accepted touch maps to a displayed source pixel. */
    int pixels = 0;
    for (int py = -1; py <= LCD35B_H; py++) for (int px = -1; px <= LCD35B_W; px++) {
        bool inside = lcd35b_point(px, py, &x, &y);
        assert(inside == (px >= 0 && px < 320 && py >= 0 && py < 480));
        if (inside) { assert(x >= 0 && x < TANK_W && y >= 0 && y < TANK_H); pixels++; }
    }
    assert(pixels == 480 * 320);
    puts("lcd35b geometry: corners, borders and all viewport pixels OK");
    return 0;
}
