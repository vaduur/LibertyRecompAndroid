#pragma once
// Compact on-screen performance overlay for Android (FPS, frame time, CPU,
// GPU, memory, temperature). Each line has its own setting, set from the
// launcher's "Performance overlay" card through files/overlay.txt.

struct ImDrawList;
struct ImFont;

namespace gta4::android {

// True when at least one overlay line is enabled (the dialog then repaints
// every frame so the numbers stay live).
bool PerfOverlayEnabled() noexcept;

// Draws the enabled lines in the top-left corner of a width x height screen.
void DrawPerfOverlay(ImDrawList* draw, ImFont* font, float width, float height) noexcept;

}  // namespace gta4::android
