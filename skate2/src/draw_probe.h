#pragma once

// Draw-translation ceiling probe.
//
// Measures how fast the game could run if turning its draws into Direct3D 12
// work cost nothing, the upper bound for any faster renderer. It switches the
// runtime's d3d12_debug_drop_draws probe (draws and resolves are skipped after
// shader analysis) and skate2_skip_draw_dispatch, which also skips the game's
// own world draw-list dispatch (sub_82364648), so the game builds no draw
// commands for that list. The picture stops updating while draws are dropped;
// the game keeps running.
//
// A key (F10 by default, bind_skate2_draw_probe) cycles: dropping fullscreen
// passes, dropping all draws, all draws plus the skipped dispatch, back to
// normal rendering with the results still shown, then hidden. A small readout
// shows the mode, the current frame rate and a table of the average frame time
// measured in each mode, compared with normal rendering.
//
// Probe settings are never kept between runs: a value saved in skate2.toml is
// reset at startup. The command line can still start in a probe mode.

#include <memory>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

class Skate2DrawProbeReadout;

// Resets probe settings that came from the configuration file and installs the
// change handling. Call once on the startup thread after the GPU plugin has
// registered its settings, before guest GPU setup.
void PrepareSkate2DrawProbe();

// Called by the guest swap hook once per guest frame: records frame times.
void NoteSkate2DrawProbeFrame();

class Skate2DrawProbe {
 public:
  explicit Skate2DrawProbe(rex::ui::ImGuiDrawer* drawer);
  ~Skate2DrawProbe();
  Skate2DrawProbe(const Skate2DrawProbe&) = delete;
  Skate2DrawProbe& operator=(const Skate2DrawProbe&) = delete;

  // Shows the readout if a probe mode is already active (command line). UI
  // thread, after the runtime is set up.
  void Start();

  // Moves to the next probe step. UI thread.
  void Cycle();

 private:
  void ShowReadout(bool show);

  rex::ui::ImGuiDrawer* drawer_;
  std::unique_ptr<Skate2DrawProbeReadout> readout_;
};
