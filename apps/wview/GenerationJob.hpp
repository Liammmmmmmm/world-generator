#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"

enum class StageStatus { Pending, Running, Done, Failed };

struct StageSlot {
    wg::StageDescription description;
    StageStatus status = StageStatus::Pending;
    float progress = 0.0f;
    // Final result when Done, latest live preview (if any) when Running.
    std::optional<wg::Snapshot> snapshot;
};

// Playback control of the simulation steps, shared by the UI and the job.
struct SimulationControl {
    std::atomic<bool> paused{false};
    std::atomic<int> stepRequests{0};       // single steps asked while paused
    std::atomic<double> stepsPerSecond{0.0}; // 0: as fast as possible
};

// Runs a pipeline on a background thread and exposes its progress, stage by
// stage, to the UI thread.
class GenerationJob {
public:
    GenerationJob(wg::Pipeline pipeline, std::shared_ptr<const wg::WorldConfig> config, double previewInterval,
                  std::shared_ptr<SimulationControl> control);
    ~GenerationJob(); // cancels and joins

    GenerationJob(const GenerationJob&) = delete;
    GenerationJob& operator=(const GenerationJob&) = delete;

    void cancel() { cancelled_ = true; }
    bool finished() const { return finished_; }

    // Copies the current state into `slots` / `error` if it changed since the
    // last call. Returns true if it did.
    bool poll(std::vector<StageSlot>& slots, std::string& error);

    // Wall-clock time of the whole generation so far.
    double elapsedSeconds() const;

    // Last log line of the stages (empty if none).
    std::string lastLog();

private:
    void update(std::size_t index, StageStatus status, float progress, std::optional<wg::Snapshot> snapshot);

    wg::Pipeline pipeline_;
    std::shared_ptr<const wg::WorldConfig> config_;
    double previewInterval_;
    std::shared_ptr<SimulationControl> control_;
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    std::string lastLog_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::atomic<double> endSeconds_{-1.0};

    std::mutex mutex_;
    std::vector<StageSlot> slots_;
    std::string error_;
    std::uint64_t version_ = 1;
    std::uint64_t polledVersion_ = 0;

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    std::thread thread_; // last: started once everything else is initialized
};
