#include "GenerationJob.hpp"

GenerationJob::GenerationJob(wg::Pipeline pipeline, std::shared_ptr<const wg::WorldConfig> config,
                             double previewInterval, std::shared_ptr<SimulationControl> control)
    : pipeline_(std::move(pipeline)), config_(std::move(config)), previewInterval_(previewInterval),
      control_(std::move(control)) {
    for (auto& description : pipeline_.stages())
        slots_.push_back({std::move(description), StageStatus::Pending, 0.0f, std::nullopt});

    thread_ = std::thread([this] {
        wg::PipelineCallbacks callbacks;
        callbacks.previewInterval = previewInterval_;
        callbacks.shouldCancel = [this] { return cancelled_.load(); };
        // Simulation playback: wait while paused (one step per request), and
        // keep to the requested speed.
        callbacks.onTick = [this](std::size_t) {
            using namespace std::chrono;
            while (control_->paused && !cancelled_) {
                int requests = control_->stepRequests;
                if (requests > 0 && control_->stepRequests.compare_exchange_weak(requests, requests - 1))
                    return true;
                std::this_thread::sleep_for(milliseconds(10));
            }
            if (const double speed = control_->stepsPerSecond; speed > 0.0 && !cancelled_) {
                const auto next = lastTick_ + duration_cast<steady_clock::duration>(duration<double>(1.0 / speed));
                while (steady_clock::now() < next && !cancelled_ && !control_->paused)
                    std::this_thread::sleep_for(milliseconds(5));
            }
            lastTick_ = steady_clock::now();
            return false;
        };
        callbacks.onLog = [this](std::size_t, const std::string& message) {
            const std::lock_guard lock(mutex_);
            lastLog_ = message;
        };
        callbacks.onStageStart = [this](std::size_t i) { update(i, StageStatus::Running, 0.0f, std::nullopt); };
        callbacks.onStageProgress = [this](std::size_t i, wg::Snapshot preview, float progress) {
            update(i, StageStatus::Running, progress, std::move(preview));
        };
        callbacks.onStageDone = [this](std::size_t i, const wg::Snapshot& result) {
            update(i, StageStatus::Done, 1.0f, result);
        };

        try {
            pipeline_.run(*config_, callbacks);
        } catch (const wg::GenerationCancelled&) {
        } catch (const std::exception& e) {
            const std::lock_guard lock(mutex_);
            for (auto& slot : slots_)
                if (slot.status == StageStatus::Running)
                    slot.status = StageStatus::Failed;
            error_ = e.what();
            ++version_;
        }
        endSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        finished_ = true;
    });
}

GenerationJob::~GenerationJob() {
    cancel();
    thread_.join();
}

void GenerationJob::update(std::size_t index, StageStatus status, float progress,
                           std::optional<wg::Snapshot> snapshot) {
    const std::lock_guard lock(mutex_);
    StageSlot& slot = slots_[index];
    slot.status = status;
    slot.progress = progress;
    if (snapshot)
        slot.snapshot = std::move(snapshot);
    ++version_;
}

bool GenerationJob::poll(std::vector<StageSlot>& slots, std::string& error) {
    const std::lock_guard lock(mutex_);
    if (polledVersion_ == version_)
        return false;
    polledVersion_ = version_;
    slots = slots_; // cheap: snapshots share their data (copy-on-write)
    error = error_;
    return true;
}

std::string GenerationJob::lastLog() {
    const std::lock_guard lock(mutex_);
    return lastLog_;
}

double GenerationJob::elapsedSeconds() const {
    const double end = endSeconds_;
    return end >= 0.0 ? end : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}
