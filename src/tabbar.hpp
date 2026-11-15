#pragma once
// A pane's tab bar and line numbers, as the cells they are drawn from. Kept
// by the pane (app.hpp declares the name); made by apptabs.cpp, and looked
// into by appsplit.cpp when a tab is dragged over another pane's bar.
#include <vector>

#include "textbox.hpp"

namespace blit {

struct TabBar {
    // The bar. Each tab's columns in it, counted from the first tab's left
    // edge (the bar may be scrolled), and where its close mark is.
    struct Span {
        int x0 = 0, x1 = 0, mark = 0;
    };
    TextBox box;
    std::vector<Span> spans;
    int plus = 0;        // the column of the "+" after the last tab
    int width = 0;       // all the tabs and the "+", in columns
    int scroll = 0;      // columns scrolled off the left
    int cols = 0;        // columns the bar has room for
    bool reveal = true;  // bring the tab in front into view at the next layout
    int drop = -1;       // where a tab being dragged in from elsewhere would go: before this tab (or after the last)
    int hover = -1;      // the tab the mouse is over (the number of tabs: the "+"); -1: none
    u64 drawn = 0;       // what the cells were made from (see tabs_layout)
    // A press on a tab, which becomes a drag once the mouse has moved.
    bool pressed = false, dragging = false;
    u64 held = 0;       // the document being held
    float press_x = 0;

    // The line numbers.
    TextBox gutter;
    i64 g_first = -1;  // the line in the gutter's first row (may be before the first line)
    u64 g_current = ~0ull, g_lines = 0;
    u32 g_font = ~0u;
    u64 g_git = ~0ull;  // the document, its changed lines and its blame, as one number
};

}  // namespace blit
