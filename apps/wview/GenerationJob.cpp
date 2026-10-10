#include "GenerationJob.hpp"

GenerationJob::GenerationJob(wg::Pipeline pipeline, std::shared_ptr<const wg::WorldConfig> config,
                             double previewInterval)
    : pipeline_(std::move(pipeline)), config_(std::move(config)), previewInterval_(previewInterval) {
    for (auto& description : pipeline_.stages())
        slots_.push_back({std::move(description), StageStatus::Pending, 0.0f, std::nullopt});

    thread_ = std::thread([this] {
        wg::PipelineCallbacks callbacks;
        callbacks.previewInterval = previewInterval_;
        callbacks.shouldCancel = [this] { return cancelled_.load(); };
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

double GenerationJob::elapsedSeconds() const {
    const double end = endSeconds_;
    return end >= 0.0 ? end : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}
