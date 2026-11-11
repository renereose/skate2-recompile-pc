#include "draw_probe.h"

#include "settings_panel.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

REXCVAR_DEFINE_BOOL(skate2_skip_draw_dispatch, false, "Skate 2",
    "Diagnostic probe, breaks the picture: skip the game's world draw-list dispatch "
    "(sub_82364648), so the game builds no draw commands for the sorted world list. Combine "
    "with d3d12_debug_drop_draws to measure the frame time left without draw work. Not kept "
    "in the configuration file. Applies immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" REX_FUNC(__imp__sub_82364648);

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kDropDrawsFlag = "d3d12_debug_drop_draws";
constexpr const char* kSkipDispatchFlag = "skate2_skip_draw_dispatch";

// Probe modes: drop setting (0 off, 1 fullscreen, 2 all) + 3 if the game's
// dispatch is skipped.
constexpr int kModeCount = 6;
constexpr int kModeAllSkipped = 5;
constexpr std::array<const char*, kModeCount> kModeNames{
    "off", "fullscreen", "all", "off + skip dispatch", "fullscreen + skip dispatch",
    "all + skip dispatch"};
constexpr std::array<const char*, kModeCount> kModeDescriptions{
    "normal rendering",
    "fullscreen passes dropped",
    "all draws dropped",
    "game's world dispatch skipped",
    "fullscreen passes dropped, game's world dispatch skipped",
    "all draws dropped, game's world dispatch skipped"};
constexpr std::array<const char*, 3> kDropValues{"off", "fullscreen", "all"};

// Frames right after a mode change are not measured: the runtime switches at
// the next frame, and caches refill when draws return.
constexpr auto kSettle = std::chrono::seconds(2);
// Each mode's result is the average over its most recent frames.
constexpr auto kWindow = std::chrono::seconds(10);
constexpr size_t kMinFrames = 30;

std::atomic<int> g_drop{0};
std::atomic<uint64_t> g_skipped_calls{0};
std::atomic<uint64_t> g_skipped_items{0};

int ParseDrop(std::string_view value) {
  if (value == "all") return 2;
  if (value == "fullscreen") return 1;
  return 0;
}

bool SkipDispatch() {
  // Written by the settings registry on the UI thread; one aligned byte.
  return *static_cast<volatile const bool*>(&REXCVAR_GET(skate2_skip_draw_dispatch));
}

int CurrentMode() {
  return g_drop.load(std::memory_order_relaxed) + (SkipDispatch() ? 3 : 0);
}

struct ModeResult {
  bool valid = false;
  double average_ms = 0.0;
  double fps = 0.0;
  double seconds = 0.0;
};

struct Snapshot {
  int mode = 0;
  bool settling = true;
  double window_seconds = 0.0;
  double window_ms = 0.0;
  double live_ms = 0.0;
  std::array<ModeResult, kModeCount> results{};
};

// Guest frame times, recorded on the guest render thread at every swap and
// read by the readout on the UI thread.
class Recorder {
 public:
  void Note() {
    const Clock::time_point now = Clock::now();
    const int mode = CurrentMode();
    std::lock_guard lock(mutex_);
    if (last_ == Clock::time_point{}) {
      last_ = now;
      mode_ = mode;
      mode_start_ = now;
      return;
    }
    const Clock::duration delta = now - last_;
    last_ = now;
    const double delta_ms = std::chrono::duration<double, std::milli>(delta).count();
    live_ms_ = live_ms_ == 0.0 ? delta_ms : live_ms_ + (delta_ms - live_ms_) * 0.05;
    if (mode != mode_) {
      Finish(now);
      mode_ = mode;
      mode_start_ = now;
      Clear();
    }
    if (now - mode_start_ < kSettle) return;
    if (count_ == kCapacity) Pop();
    const size_t index = (head_ + count_) % kCapacity;
    times_[index] = now;
    deltas_[index] = delta;
    sum_ += delta;
    ++count_;
    while (count_ > 1 && now - times_[head_] > kWindow) Pop();
  }

  Snapshot Read() {
    std::lock_guard lock(mutex_);
    Snapshot snapshot;
    snapshot.mode = mode_ < 0 ? 0 : mode_;
    snapshot.settling = count_ < kMinFrames;
    snapshot.window_seconds = std::chrono::duration<double>(sum_).count();
    snapshot.window_ms =
        count_ ? std::chrono::duration<double, std::milli>(sum_).count() / double(count_) : 0.0;
    snapshot.live_ms = live_ms_;
    snapshot.results = results_;
    if (!snapshot.settling) {
      // The mode being measured shows its running value.
      snapshot.results[snapshot.mode] = Result();
    }
    return snapshot;
  }

  // Logs the mode being measured without leaving it (when the readout is
  // hidden).
  void LogCurrent() {
    std::lock_guard lock(mutex_);
    if (mode_ < 0 || mode_ >= kModeCount) return;
    const ModeResult result = Result();
    if (!result.valid) return;
    results_[mode_] = result;
    Log(mode_, result, std::string());
  }

 private:
  static constexpr size_t kCapacity = 16384;

  ModeResult Result() const {
    ModeResult result;
    if (count_ < kMinFrames) return result;
    result.valid = true;
    result.seconds = std::chrono::duration<double>(sum_).count();
    result.average_ms = result.seconds * 1000.0 / double(count_);
    result.fps = double(count_) / result.seconds;
    return result;
  }

  void Finish(Clock::time_point now) {
    if (mode_ < 0 || mode_ >= kModeCount) return;
    const ModeResult result = Result();
    const uint64_t calls = g_skipped_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t items = g_skipped_items.exchange(0, std::memory_order_relaxed);
    if (!result.valid) {
      REXLOG_INFO("Skate 2 draw probe: left mode '{}' after {:.1f} s, too short to measure",
                  kModeNames[mode_], std::chrono::duration<double>(now - mode_start_).count());
      return;
    }
    results_[mode_] = result;
    std::string skipped;
    if (calls) {
      skipped = fmt::format(" (game dispatch skipped {} times, {:.0f} world list entries per call)",
                            calls, double(items) / double(calls));
    }
    Log(mode_, result, skipped);
  }

  void Log(int mode, const ModeResult& result, const std::string& suffix) const {
    const ModeResult& off = results_[0];
    std::string versus;
    if (mode != 0 && off.valid) {
      versus = fmt::format(", {:+.0f}% frame time vs off",
                           (result.average_ms / off.average_ms - 1.0) * 100.0);
    }
    REXLOG_INFO("Skate 2 draw probe result: {}: {:.2f} ms average, {:.1f} FPS over the last "
                "{:.1f} s{}{}",
                kModeNames[mode], result.average_ms, result.fps, result.seconds, versus, suffix);
  }

  void Pop() {
    sum_ -= deltas_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }

  void Clear() {
    head_ = 0;
    count_ = 0;
    sum_ = Clock::duration::zero();
  }

  std::mutex mutex_;
  Clock::time_point last_{};
  int mode_ = -1;
  Clock::time_point mode_start_{};
  double live_ms_ = 0.0;
  std::array<Clock::time_point, kCapacity> times_{};
  std::array<Clock::duration, kCapacity> deltas_{};
  size_t head_ = 0;
  size_t count_ = 0;
  Clock::duration sum_ = Clock::duration::zero();
  std::array<ModeResult, kModeCount> results_{};
};

Recorder g_recorder;

bool FlagExists(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

// Probe values must not outlive a run: a saved "all" would start the next run
// with a frozen picture.
void ResetSavedValue(const char* name, const char* off) {
  if (!FlagExists(name) || rex::cvar::GetFlagSource(name) != rex::cvar::Source::kConfig) return;
  const std::string saved = rex::cvar::GetFlagByName(name);
  if (saved == off) return;
  if (rex::cvar::SetFlagByName(name, off)) {
    REXLOG_WARN("Skate 2 draw probe: ignoring {}={} from the configuration file; probe settings "
                "apply only to the run they are set in",
                name, saved);
  }
}

}  // namespace

// The game's world draw-list dispatcher: r3 = scene view, r4 = first entry,
// r5 = entry count of the sorted list at view+20160. It binds each entry's
// shader, material and pass state and draws it through sub_82364240, and draws
// multi-pass entries afterwards from a list it rebuilds at the start of every
// call. Its only caller (sub_8237B398, the view render) uses nothing it
// returns, and the bound-state caches it updates are invalidated by that
// caller beforehand, so skipping it leaves no state behind.
extern "C" REX_FUNC(sub_82364648) {
  if (SkipDispatch()) [[unlikely]] {
    g_skipped_calls.fetch_add(1, std::memory_order_relaxed);
    g_skipped_items.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82364648(ctx, base);
}

void PrepareSkate2DrawProbe() {
  ResetSavedValue(kDropDrawsFlag, "off");
  ResetSavedValue(kSkipDispatchFlag, "false");
  static std::once_flag installed;
  std::call_once(installed, [] {
    if (FlagExists(kDropDrawsFlag)) {
      rex::cvar::RegisterChangeCallback(kDropDrawsFlag, [](std::string_view, std::string_view value) {
        g_drop.store(ParseDrop(value), std::memory_order_relaxed);
      });
    }
  });
  g_drop.store(FlagExists(kDropDrawsFlag) ? ParseDrop(rex::cvar::GetFlagByName(kDropDrawsFlag)) : 0,
               std::memory_order_relaxed);
  if (!FlagExists(kDropDrawsFlag)) {
    REXLOG_WARN("Skate 2 draw probe: this runtime has no d3d12_debug_drop_draws; only the game "
                "dispatch skip is available");
  }
}

void NoteSkate2DrawProbeFrame() { g_recorder.Note(); }

// ---- Readout ------------------------------------------------------------------

class Skate2DrawProbeReadout final : public rex::ui::ImGuiDialog {
 public:
  explicit Skate2DrawProbeReadout(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

  // Repainted with every guest frame; nothing here animates on its own.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Snapshot snapshot = g_recorder.Read();
    const float font_size = std::clamp(io.DisplaySize.y / 54.0f, 14.0f, 32.0f);
    ImGui::PushFont(Skate2SettingsPanel::FontForSize(font_size), font_size);
    const float margin = font_size;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.8f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##skate2_draw_probe", nullptr, kFlags)) {
      const ImVec4 accent(1.0f, 0.8f, 0.3f, 1.0f);
      const ImVec4 dim(0.7f, 0.7f, 0.7f, 1.0f);
      ImGui::TextColored(accent, "Draw probe: %s", kModeDescriptions[snapshot.mode]);
      if (snapshot.live_ms > 0.0) {
        ImGui::Text("Now %.1f FPS, %.2f ms", 1000.0 / snapshot.live_ms, snapshot.live_ms);
      }
      if (snapshot.settling) {
        ImGui::TextColored(dim, "Measuring this mode...");
      } else {
        ImGui::Text("This mode: %.2f ms average over %.0f s", snapshot.window_ms,
                    snapshot.window_seconds);
      }
      ImGui::Separator();
      const ModeResult& off = snapshot.results[0];
      if (ImGui::BeginTable("##results", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("FPS");
        ImGui::TableSetupColumn("vs off");
        ImGui::TableHeadersRow();
        for (int mode = 0; mode < kModeCount; ++mode) {
          const ModeResult& result = snapshot.results[mode];
          // The cycle's modes always; others only when measured.
          const bool in_cycle = mode <= 2 || mode == kModeAllSkipped;
          if (!result.valid && !in_cycle) continue;
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (mode == snapshot.mode) {
            ImGui::TextColored(accent, "> %s", kModeNames[mode]);
          } else {
            ImGui::Text("  %s", kModeNames[mode]);
          }
          ImGui::TableNextColumn();
          if (result.valid) {
            ImGui::Text("%.2f", result.average_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", result.fps);
            ImGui::TableNextColumn();
            if (mode != 0 && off.valid) {
              ImGui::Text("%+.0f%%", (result.average_ms / off.average_ms - 1.0) * 100.0);
            } else {
              ImGui::TextColored(dim, "-");
            }
          } else {
            ImGui::TextColored(dim, "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(dim, "-");
          }
        }
        ImGui::EndTable();
      }
      const std::string key = rex::cvar::GetFlagByName("bind_skate2_draw_probe");
      ImGui::TextColored(dim, "%s: next mode. The picture freezes while draws are dropped.",
                         key.empty() ? "F10" : key.c_str());
      ImGui::TextColored(dim, "Stay in one spot; each mode measures its last 10 s.");
    }
    ImGui::End();
    ImGui::PopFont();
  }
};

// ---- Probe control ------------------------------------------------------------

Skate2DrawProbe::Skate2DrawProbe(rex::ui::ImGuiDrawer* drawer) : drawer_(drawer) {}

Skate2DrawProbe::~Skate2DrawProbe() = default;

void Skate2DrawProbe::Start() {
  if (CurrentMode() != 0) ShowReadout(true);
}

void Skate2DrawProbe::ShowReadout(bool show) {
  if (show && !readout_ && drawer_) {
    readout_ = std::make_unique<Skate2DrawProbeReadout>(drawer_);
  } else if (!show) {
    readout_.reset();
  }
}

void Skate2DrawProbe::Cycle() {
  // hidden -> fullscreen -> all -> all + skip dispatch -> off with the results
  // shown -> hidden.
  const int mode = CurrentMode();
  int next = 0;
  bool show = true;
  if (!readout_) {
    next = 1;
  } else {
    switch (mode) {
      case 0:
        show = false;
        break;
      case 1:
        next = 2;
        break;
      case 2:
        next = kModeAllSkipped;
        break;
      default:
        next = 0;
        break;
    }
  }
  if (!show) g_recorder.LogCurrent();
  const int drop = next % 3;
  const bool skip = next >= 3;
  if (FlagExists(kDropDrawsFlag)) rex::cvar::SetFlagByName(kDropDrawsFlag, kDropValues[drop]);
  rex::cvar::SetFlagByName(kSkipDispatchFlag, skip ? "true" : "false");
  REXLOG_INFO("Skate 2 draw probe: {}", show ? kModeDescriptions[CurrentMode()] : "hidden");
  ShowReadout(show);
}
