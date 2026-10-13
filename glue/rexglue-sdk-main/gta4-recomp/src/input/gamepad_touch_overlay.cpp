#include "input/gamepad_touch_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_touch.h>

#include <rex/cvar.h>
#include <rex/input/absolute_pointer.h>
#include <rex/input/input.h>

REXCVAR_DEFINE_STRING(gta4_touch_layout, "gamepad", "GTA IV/Input/Touch",
                      "On-screen touch controls: gamepad (a fixed Xbox 360 controller) or context "
                      "(the context-sensitive layout)")
    .allowed({"gamepad", "context"});
REXCVAR_DEFINE_STRING(gta4_touch_camera, "swipe", "GTA IV/Input/Touch",
                      "How the right half of the screen moves the camera: swipe (the camera "
                      "follows your finger, like a mouse) or stick (a virtual joystick)")
    .allowed({"swipe", "stick"});
REXCVAR_DEFINE_DOUBLE(gta4_touch_swipe_sensitivity, 1.0, "GTA IV/Input/Touch",
                      "Swipe camera speed; 0.5 is half as fast, 2.0 twice as fast");
REXCVAR_DEFINE_DOUBLE(gta4_touch_gamepad_scale, 1.0, "GTA IV/Input/Touch",
                      "Size of the on-screen gamepad's buttons (0.75 to 1.35)");
REXCVAR_DEFINE_DOUBLE(gta4_touch_gamepad_opacity, 1.0, "GTA IV/Input/Touch",
                      "Opacity of the on-screen gamepad (0.3 faint to 1.6 strong)");

namespace gta4::input {
namespace {

enum class Kind : uint8_t { kStick, kButton, kTrigger, kArrow };

struct Control {
  Kind kind = Kind::kButton;
  float x = 0.0f, y = 0.0f, r = 0.0f;  // display pixels; a stick's resting position
  uint16_t button = 0;                  // buttons, arrows
  uint8_t index = 0;                    // stick 0/1, trigger 0/1, arrow 0..3 (up, down, left, right)
  const char* label = "";
  ImU32 label_color = 0;                // 0: the common label colour
};

constexpr size_t kControlCount = 20;
constexpr size_t kLeftStick = 0;
constexpr size_t kRightStick = 1;
constexpr float kDeadZone = 0.12f;

// Grey and see-through, so the overlay does not hide the game.
constexpr ImU32 kFill = IM_COL32(90, 90, 90, 55);
constexpr ImU32 kFillHeld = IM_COL32(200, 200, 200, 105);
constexpr ImU32 kRing = IM_COL32(215, 215, 215, 105);
constexpr ImU32 kLabel = IM_COL32(230, 230, 230, 150);
constexpr ImU32 kKnob = IM_COL32(225, 225, 225, 130);

// The colours above with their alpha scaled by gta4_touch_gamepad_opacity.
ImU32 Faded(ImU32 color) {
  const float opacity = float(std::clamp(REXCVAR_GET(gta4_touch_gamepad_opacity), 0.3, 1.6));
  const uint32_t alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
  const uint32_t scaled = uint32_t(std::clamp(alpha * opacity, 0.0f, 255.0f));
  return (color & ~IM_COL32_A_MASK) | (scaled << IM_COL32_A_SHIFT);
}

struct Finger {
  int control = -1;
};

struct State {
  std::mutex mutex;
  bool visible = false;
  std::array<Control, kControlCount> controls{};
  std::array<bool, kControlCount> held{};
  // Sticks float: the centre is where the finger came down.
  ImVec2 stick_origin[2] = {{0, 0}, {0, 0}};
  float stick_x[2] = {0, 0};  // -1..1, screen space (y down)
  float stick_y[2] = {0, 0};
  std::unordered_map<uint64_t, Finger> fingers;
  // Swipe camera: the finger's last position and the time of the last update,
  // and whether the right stick is currently driven by a swipe.
  ImVec2 swipe_last = {0, 0};
  bool swipe_tracking = false;
  bool swipe_active = false;
  std::chrono::steady_clock::time_point last_update{};
};

bool SwipeCamera() noexcept {
  return REXCVAR_GET(gta4_touch_camera) != "stick";
}

State& state() {
  static State s;
  return s;
}

// Sized from the screen height so the controls keep their physical size on
// 16:9 and 20:9 panels. The left half of the screen outside the buttons is the
// left stick (it centres wherever the finger lands); the right half is the
// right stick, i.e. the camera. The radar moves to the top-left corner while
// the overlay is up (ContextTouchHudLayoutActive).
std::array<Control, kControlCount> BuildLayout(float w, float h) {
  std::array<Control, kControlCount> c{};
  size_t n = 0;
  const auto add = [&](Kind kind, float x, float y, float r, uint16_t button, uint8_t index,
                       const char* label, ImU32 color = 0) {
    c[n++] = Control{kind, x, y, r, button, index, label, color};
  };
  const float lx = 0.12f * w;
  // gta4_touch_gamepad_scale grows or shrinks the buttons and the spacing
  // inside each group (D-pad, face buttons, shoulder pairs) around fixed
  // anchors, so the groups stay where thumbs expect them.
  const float k = float(std::clamp(REXCVAR_GET(gta4_touch_gamepad_scale), 0.75, 1.35));
  // Sticks first: kLeftStick, kRightStick. The right stick has no resting
  // place; it appears where the camera finger lands.
  add(Kind::kStick, lx, 0.72f * h, 0.105f * h * k, 0, 0, "L");
  add(Kind::kStick, w * 0.75f, 0.60f * h, 0.095f * h, 0, 1, "R");
  // D-pad, right of the left stick.
  const float dx = lx + 0.26f * h, dy = 0.86f * h, arm = 0.078f * h * k, dr = 0.042f * h * k;
  add(Kind::kArrow, dx, dy - arm, dr, rex::input::X_INPUT_GAMEPAD_DPAD_UP, 0, "");
  add(Kind::kArrow, dx, dy + arm, dr, rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, 1, "");
  add(Kind::kArrow, dx - arm, dy, dr, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, 2, "");
  add(Kind::kArrow, dx + arm, dy, dr, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, 3, "");
  // LT over LB above the left stick, L3 beside LB.
  const float br = 0.048f * h * k, small = 0.036f * h * k;
  add(Kind::kTrigger, lx, 0.355f * h, br, 0, 0, "LT");
  add(Kind::kButton, lx, 0.475f * h, br, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, "LB");
  add(Kind::kButton, lx + 0.115f * h * k, 0.475f * h, small, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, 0,
      "L3");
  // A/B/X/Y diamond in the lower right, with the Xbox 360 letter colours.
  const float fx = w - 0.52f * h, fy = 0.80f * h, fs = 0.097f * h * k, fr = 0.052f * h * k;
  add(Kind::kButton, fx, fy + fs, fr, rex::input::X_INPUT_GAMEPAD_A, 0, "A",
      IM_COL32(110, 205, 80, 170));
  add(Kind::kButton, fx + fs, fy, fr, rex::input::X_INPUT_GAMEPAD_B, 0, "B",
      IM_COL32(235, 75, 70, 170));
  add(Kind::kButton, fx - fs, fy, fr, rex::input::X_INPUT_GAMEPAD_X, 0, "X",
      IM_COL32(75, 145, 240, 170));
  add(Kind::kButton, fx, fy - fs, fr, rex::input::X_INPUT_GAMEPAD_Y, 0, "Y",
      IM_COL32(245, 205, 50, 170));
  // RT over RB at the right edge, R3 beside RB.
  const float rx = w - 0.17f * h;
  add(Kind::kTrigger, rx, 0.47f * h, br, 0, 1, "RT");
  add(Kind::kButton, rx, 0.59f * h, br, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, "RB");
  add(Kind::kButton, rx - 0.115f * h * k, 0.59f * h, small, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, 0,
      "R3");
  // Back and Start in the middle of the top edge.
  const float sr = 0.036f * h * k;
  add(Kind::kButton, 0.44f * w, 0.075f * h, sr, rex::input::X_INPUT_GAMEPAD_BACK, 0, "BACK");
  add(Kind::kButton, 0.56f * w, 0.075f * h, sr, rex::input::X_INPUT_GAMEPAD_START, 0, "START");
  return c;
}

// The button under a finger, or the stick of that half of the screen.
int HitTest(const std::array<Control, kControlCount>& controls, float px, float py, float w) {
  int best = -1;
  float best_distance = FLT_MAX;
  for (size_t i = 0; i < controls.size(); ++i) {
    const auto& control = controls[i];
    if (control.r <= 0.0f || control.kind == Kind::kStick) continue;
    // A finger that lands just outside a button still presses it; the nearest
    // button (relative to its size) wins where two capture areas overlap.
    const float capture = control.r * 1.4f;
    const float distance = std::hypot(px - control.x, py - control.y);
    if (distance <= capture && distance / capture < best_distance) {
      best_distance = distance / capture;
      best = int(i);
    }
  }
  if (best >= 0) return best;
  return int(px < w * 0.5f ? kLeftStick : kRightStick);
}

// The origin that draws a label's ink centred on `center`: horizontally by its
// advance, vertically by the glyph box of its first letter (labels are capitals),
// not by the line height, which leaves letters sitting high.
ImVec2 LabelOrigin(ImFont* font, float size, const char* label, ImVec2 center) {
  const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
  float y = center.y - text.y * 0.5f;
  float x = center.x - text.x * 0.5f;
  if (ImFontBaked* baked = font->GetFontBaked(size)) {
    if (const ImFontGlyph* glyph = baked->FindGlyphNoFallback(ImWchar(label[0]))) {
      const float scale = baked->Size > 0.0f ? size / baked->Size : 1.0f;
      y = center.y - (glyph->Y0 + glyph->Y1) * 0.5f * scale;
      if (label[1] == 0) x = center.x - (glyph->X0 + glyph->X1) * 0.5f * scale;
    }
  }
  return ImVec2(x, y);
}

void Draw(ImDrawList* draw, ImFont* font, float font_size, const State& s) {
  for (size_t i = 0; i < s.controls.size(); ++i) {
    const auto& control = s.controls[i];
    if (control.r <= 0.0f) continue;
    const bool active = s.held[i];
    const float radius = control.r;
    if (control.kind == Kind::kStick) {
      // The camera stick shows only while it is being used.
      if (control.index == 1 && (!active || SwipeCamera())) continue;
      const ImVec2 center = active ? s.stick_origin[control.index] : ImVec2(control.x, control.y);
      draw->AddCircleFilled(center, radius, Faded(kFill), 48);
      draw->AddCircle(center, radius, Faded(kRing), 48, std::max(1.0f, radius * 0.025f));
      const ImVec2 knob(center.x + s.stick_x[control.index] * radius * 0.55f,
                        center.y + s.stick_y[control.index] * radius * 0.55f);
      draw->AddCircleFilled(knob, radius * 0.34f, Faded(kKnob), 32);
      continue;
    }
    const ImVec2 center(control.x, control.y);
    draw->AddCircleFilled(center, radius, Faded(active ? kFillHeld : kFill), 48);
    draw->AddCircle(center, radius, Faded(kRing), 48, std::max(1.0f, radius * 0.025f));
    if (control.kind == Kind::kArrow) {
      const float t = radius * 0.42f;
      ImVec2 a, b, c;
      switch (control.index) {
        case 0: a = {center.x, center.y - t}; b = {center.x - t, center.y + t * 0.6f}; c = {center.x + t, center.y + t * 0.6f}; break;
        case 1: a = {center.x, center.y + t}; b = {center.x + t, center.y - t * 0.6f}; c = {center.x - t, center.y - t * 0.6f}; break;
        case 2: a = {center.x - t, center.y}; b = {center.x + t * 0.6f, center.y + t}; c = {center.x + t * 0.6f, center.y - t}; break;
        default: a = {center.x + t, center.y}; b = {center.x - t * 0.6f, center.y - t}; c = {center.x - t * 0.6f, center.y + t}; break;
      }
      draw->AddTriangleFilled(a, b, c, Faded(kLabel));
      continue;
    }
    // Letters fill most of the circle; longer labels are fitted to its width.
    const ImVec2 natural = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, control.label);
    const float size = control.label[1] == 0 ? radius * 0.95f
        : natural.x > 0.0f ? std::min(radius * 0.62f, font_size * radius * 1.35f / natural.x)
                           : font_size;
    draw->AddText(font, size, LabelOrigin(font, size, control.label, center),
                  Faded(control.label_color ? control.label_color : kLabel), control.label);
  }
}

}  // namespace

bool GamepadTouchLayoutSelected() noexcept {
  return REXCVAR_GET(gta4_touch_layout) == "gamepad";
}

bool GamepadTouchOverlayVisible() noexcept {
  if (!GamepadTouchLayoutSelected()) return false;
  std::lock_guard lock(state().mutex);
  return state().visible;
}

bool UpdateAndDrawGamepadTouchOverlay(ImDrawList* draw, ImFont* font, float font_size,
                                      float width, float height) noexcept {
  auto& s = state();
  const bool visible = GamepadTouchLayoutSelected() && rex::input::TouchControlsVisible() &&
                       width > 0.0f && height > 0.0f;
  std::lock_guard lock(s.mutex);
  if (!visible) {
    s.visible = false;
    s.fingers.clear();
    s.held.fill(false);
    s.stick_x[0] = s.stick_x[1] = s.stick_y[0] = s.stick_y[1] = 0.0f;
    s.swipe_tracking = s.swipe_active = false;
    return false;
  }
  s.visible = true;
  s.controls = BuildLayout(width, height);
  const auto now = std::chrono::steady_clock::now();
  const float frame_seconds =
      s.last_update.time_since_epoch().count() == 0
          ? 1.0f / 60.0f
          : std::chrono::duration<float>(now - s.last_update).count();
  s.last_update = now;

  // Every finger currently on the screen, from every touch device.
  std::unordered_map<uint64_t, ImVec2> touches;
  int device_count = 0;
  if (SDL_TouchID* devices = SDL_GetTouchDevices(&device_count)) {
    for (int d = 0; d < device_count; ++d) {
      int finger_count = 0;
      if (SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &finger_count)) {
        for (int f = 0; f < finger_count; ++f) {
          const uint64_t key = (uint64_t(devices[d]) << 32) ^ uint64_t(fingers[f]->id);
          touches[key] = ImVec2(fingers[f]->x * width, fingers[f]->y * height);
        }
        SDL_free(fingers);
      }
    }
    SDL_free(devices);
  }

  for (auto it = s.fingers.begin(); it != s.fingers.end();) {
    it = touches.count(it->first) ? std::next(it) : s.fingers.erase(it);
  }
  bool stick_taken[2] = {false, false};
  for (const auto& [key, finger] : s.fingers) {
    if (finger.control >= 0 && s.controls[finger.control].kind == Kind::kStick) {
      stick_taken[s.controls[finger.control].index] = true;
    }
  }

  s.held.fill(false);
  bool stick_owned[2] = {false, false};
  for (const auto& [key, position] : touches) {
    auto found = s.fingers.find(key);
    if (found == s.fingers.end()) {
      // A finger belongs to what it first landed on until it lifts. A stick
      // centres under the finger, kept fully on screen; one finger per stick.
      int control = HitTest(s.controls, position.x, position.y, width);
      if (control >= 0 && s.controls[control].kind == Kind::kStick) {
        const auto& stick = s.controls[control];
        if (stick_taken[stick.index]) {
          control = -1;
        } else {
          stick_taken[stick.index] = true;
          s.stick_origin[stick.index] =
              ImVec2(std::clamp(position.x, stick.r, width - stick.r),
                     std::clamp(position.y, stick.r, height - stick.r));
        }
      }
      found = s.fingers.emplace(key, Finger{control}).first;
    }
    const int control_index = found->second.control;
    if (control_index < 0) continue;
    const Control& control = s.controls[control_index];
    s.held[control_index] = true;
    if (control.kind != Kind::kStick) continue;
    if (control.index == kRightStick && SwipeCamera()) {
      // Swipe camera: the stick follows the finger's speed, so the view turns
      // while the finger moves and stops when it stops. Full speed when the
      // finger crosses about 1.5 screen heights per second (sensitivity 1).
      if (!s.swipe_tracking) {
        s.swipe_last = position;
        s.swipe_tracking = true;
      }
      const float dt = std::clamp(frame_seconds, 1.0f / 240.0f, 0.1f);
      const float sensitivity = float(std::clamp(REXCVAR_GET(gta4_touch_swipe_sensitivity), 0.1, 5.0));
      const float full_speed = height * 1.5f / sensitivity;
      const float target_x = std::clamp((position.x - s.swipe_last.x) / dt / full_speed, -1.0f, 1.0f);
      const float target_y = std::clamp((position.y - s.swipe_last.y) / dt / full_speed, -1.0f, 1.0f);
      // Light smoothing: touch samples arrive unevenly between frames.
      s.stick_x[kRightStick] += (target_x - s.stick_x[kRightStick]) * 0.6f;
      s.stick_y[kRightStick] += (target_y - s.stick_y[kRightStick]) * 0.6f;
      s.swipe_last = position;
      s.swipe_active = true;
      stick_owned[kRightStick] = true;
      continue;
    }
    const ImVec2 origin = s.stick_origin[control.index];
    float x = (position.x - origin.x) / (control.r * 0.85f);
    float y = (position.y - origin.y) / (control.r * 0.85f);
    const float length = std::hypot(x, y);
    if (length > 1.0f) {
      x /= length;
      y /= length;
    }
    s.stick_x[control.index] = x;
    s.stick_y[control.index] = y;
    stick_owned[control.index] = true;
  }
  for (int i = 0; i < 2; ++i) {
    if (!stick_owned[i]) s.stick_x[i] = s.stick_y[i] = 0.0f;
  }
  if (!stick_owned[kRightStick]) s.swipe_tracking = s.swipe_active = false;

  if (draw && font && font_size > 0.0f) Draw(draw, font, font_size, s);
  return true;
}

bool ReadGamepadTouchOverlay(uint32_t user, rex::input::X_INPUT_GAMEPAD* output) noexcept {
  if (!output || user != 0) return false;
  auto& s = state();
  std::lock_guard lock(s.mutex);
  if (!s.visible) return false;
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  for (size_t i = 0; i < s.controls.size(); ++i) {
    const auto& control = s.controls[i];
    if (!s.held[i]) continue;
    if (control.kind == Kind::kTrigger) {
      triggers[control.index] = 255;
    } else if (control.kind != Kind::kStick) {
      buttons |= control.button;
    }
  }
  // Radial dead zone per stick; screen Y points down, the pad's up.
  const auto stick = [&](int i, int16_t* x_out, int16_t* y_out) {
    const float x = s.stick_x[i], y = s.stick_y[i];
    const float length = std::hypot(x, y);
    if (length < kDeadZone) {
      *x_out = *y_out = 0;
      return;
    }
    const float scale = std::min((length - kDeadZone) / (1.0f - kDeadZone), 1.0f) / length;
    *x_out = int16_t(std::clamp(x * scale, -1.0f, 1.0f) * 32767.0f);
    *y_out = int16_t(std::clamp(-y * scale, -1.0f, 1.0f) * 32767.0f);
  };
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  stick(0, &lx, &ly);
  if (s.swipe_active) {
    // Swipe camera: no dead zone of our own, and small movements start just
    // past the game's own right-stick dead zone (about 27%, as on XInput) so
    // slow, fine aiming swipes still turn the camera.
    constexpr float kGameDeadZone = 0.27f;
    const float x = s.stick_x[1], y = s.stick_y[1];
    const float length = std::hypot(x, y);
    // Leftovers of the smoothing after the finger stops are dropped, or the
    // dead-zone offset would keep the camera creeping for a few frames.
    if (length > 0.04f) {
      const float scaled = std::min(kGameDeadZone + (1.0f - kGameDeadZone) * length, 1.0f) / length;
      rx = int16_t(std::clamp(x * scaled, -1.0f, 1.0f) * 32767.0f);
      ry = int16_t(std::clamp(-y * scaled, -1.0f, 1.0f) * 32767.0f);
    }
  } else {
    stick(1, &rx, &ry);
  }
  *output = {};
  output->buttons = buttons;
  output->left_trigger = triggers[0];
  output->right_trigger = triggers[1];
  output->thumb_lx = lx;
  output->thumb_ly = ly;
  output->thumb_rx = rx;
  output->thumb_ry = ry;
  return true;
}

}  // namespace gta4::input
