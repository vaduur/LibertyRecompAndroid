#pragma once
// Android Performance Hint (ADPF): tells the system how long the game's frames
// take against a 30 FPS target, so it raises the CPU clocks of the game's
// busiest threads when frames run late. Without it, phones whose power manager
// does not recognise the app as a game can leave every core at low clocks.

namespace gta4::android {

// Called by the title's frame-wait hook each time the game submits a frame.
void NoteTitleFrame() noexcept;

// Starts the background thread that manages the hint session.
void StartPerformanceHint();

}  // namespace gta4::android
