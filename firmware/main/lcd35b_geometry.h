/* Native portrait panel -> 3:2 landscape world. Keep the whole UI and
 * aspect ratio: 552 x 368 world fills the 480 x 320 landscape LCD.
 * These integer maps are shared by display and touch to keep them aligned. */
#ifndef LCD35B_GEOMETRY_H
#define LCD35B_GEOMETRY_H
#include <stdbool.h>
#define LCD35B_W 320
#define LCD35B_H 480
#define LCD35B_VIEW_W 480
#define LCD35B_VIEW_X 0
static inline bool lcd35b_point(int px, int py, int *tx, int *ty) {
    int x = LCD35B_H - 1 - py - LCD35B_VIEW_X;
    if (px < 0 || px >= LCD35B_W || x < 0 || x >= LCD35B_VIEW_W) return false;
    *tx = x * (TANK_W - 1) / (LCD35B_VIEW_W - 1);
    *ty = px * (TANK_H - 1) / (LCD35B_W - 1);
    return true;
}
#endif
