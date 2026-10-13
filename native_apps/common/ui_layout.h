/**
 * UI Layout Manager for Arca
 * 
 * Provides layout managers for:
 * - Grid layout (auto-calculate positions, handle overflow)
 * - List layout (scrollable vertical list)
 * - Flow layout (auto-wrap items)
 */

#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "framebuffer.h"
#include <stdint.h>
#include <stdbool.h>

// Layout types
typedef enum {
    LAYOUT_GRID,
    LAYOUT_LIST,
    LAYOUT_FLOW
} LayoutType;

// Grid layout configuration
typedef struct {
    int columns;              // Number of columns
    int item_width;           // Width of each item
    int item_height;          // Height of each item
    int spacing_x;            // Horizontal spacing
    int spacing_y;            // Vertical spacing
    int margin_left;          // Left margin
    int margin_top;           // Top margin
    int margin_right;         // Right margin
    int margin_bottom;        // Bottom margin
} GridLayoutConfig;

// List layout configuration
typedef struct {
    int item_height;          // Height of each item
    int spacing;              // Spacing between items
    int margin_left;          // Left margin
    int margin_top;           // Top margin
    int margin_right;         // Right margin
    int margin_bottom;        // Bottom margin
    int visible_items;        // Number of items visible at once
} ListLayoutConfig;

// Layout manager
typedef struct {
    LayoutType type;
    int screen_width;
    int screen_height;
    
    union {
        GridLayoutConfig grid;
        ListLayoutConfig list;
    } config;
    
    // Scrolling state
    int scroll_offset;        // Current scroll position
    int total_items;          // Total number of items
    int visible_items;        // Number of items currently visible
    bool can_scroll_up;       // Can scroll up
    bool can_scroll_down;     // Can scroll down
} UILayout;

// Initialize grid layout
void ui_layout_init_grid(UILayout *layout, int screen_width, int screen_height,
                        int columns, int item_width, int item_height,
                        int spacing_x, int spacing_y,
                        int margin_left, int margin_top, int margin_right, int margin_bottom);

// Initialize list layout
void ui_layout_init_list(UILayout *layout, int screen_width, int screen_height,
                        int item_height, int spacing,
                        int margin_left, int margin_top, int margin_right, int margin_bottom);

// Update layout with item count
void ui_layout_update(UILayout *layout, int total_items);

// Get position for item at index
bool ui_layout_get_item_position(UILayout *layout, int index, int *x, int *y, int *width, int *height);

// Check if item at index is visible
bool ui_layout_is_item_visible(UILayout *layout, int index);

// Scroll up (returns true if scrolled)
bool ui_layout_scroll_up(UILayout *layout);

// Scroll down (returns true if scrolled)
bool ui_layout_scroll_down(UILayout *layout);

// Get item index at touch position (returns -1 if none)
int ui_layout_get_item_at_position(UILayout *layout, int touch_x, int touch_y);

// Draw scroll indicators
void ui_layout_draw_scroll_indicators(Framebuffer *fb, UILayout *layout);

// Helper: Calculate grid layout dimensions
void ui_layout_grid_calculate(GridLayoutConfig *config, int screen_width, int screen_height,
                              int total_items, int *out_rows, int *out_cols);

// Helper: Calculate list layout dimensions
void ui_layout_list_calculate(ListLayoutConfig *config, int screen_height,
                              int total_items, int *out_visible);

#endif
