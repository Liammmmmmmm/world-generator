// wview: interactive, live preview of a world generation, stage by stage.
//
//   wview <config.json | world.wgw>
//
// With a config file, the pipeline runs on a background thread: stages show up
// in the list as they run, and the running stage can be watched while it works.
// Everything is regenerated each time the config file is saved.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include "GenerationJob.hpp"
#include "Meshes.hpp"
#include "wg/core/Config.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/render/MapRenderer.hpp"

namespace fs = std::filesystem;

namespace {

constexpr Color kBackground{24, 26, 30, 255};
constexpr Color kText{230, 230, 230, 255};
constexpr Color kDimText{150, 150, 155, 255};
constexpr Color kPendingText{100, 100, 105, 255};
constexpr Color kPanel{0, 0, 0, 170};
constexpr Color kHighlight{70, 110, 200, 200};
constexpr Color kProgress{90, 170, 110, 255};
constexpr Color kError{255, 100, 90, 255};

constexpr int kFont = 18;
constexpr int kLine = 22;
constexpr int kPanelWidth = 330;
constexpr int kMargin = 8;

// How often a running stage sends a live preview (each one is re-rendered).
constexpr double kPreviewInterval = 0.1;

enum class ViewMode { Map2D, Terrain3D };

class Viewer {
public:
    explicit Viewer(fs::path source) : source_(std::move(source)), isConfig_(source_.extension() == ".json") {}

    ~Viewer() {
        job_.reset();
        retiredJobs_.clear();
        clearGpuCache();
    }

    void run() {
        reload();
        while (!WindowShouldClose()) {
            syncJob();
            handleInput();
            watchSource();

            BeginDrawing();
            ClearBackground(kBackground);
            drawWorld();
            drawPanel();
            drawStatusBar();
            EndDrawing();
        }
    }

private:
    // --- Generation ------------------------------------------------------------

    void reload() {
        try {
            wg::WorldInfo info;
            if (isConfig_) {
                const auto config = wg::WorldConfig::load(source_);
                auto pipeline = wg::Pipeline::fromConfig(config);
                info = config.info();
                // The previous job may still be running a stage that never reports
                // progress: let it finish in the background instead of blocking.
                if (job_) {
                    job_->cancel();
                    retiredJobs_.push_back(std::move(job_));
                }
                job_ = std::make_unique<GenerationJob>(std::move(pipeline), info, kPreviewInterval);
            } else {
                wg::Snapshot snapshot;
                snapshot.state = wg::readWorld(source_);
                snapshot.stageName = source_.filename().string();
                snapshot.stageType = "file";
                snapshot.changedLayers = snapshot.state.layerNames();
                snapshot.voxelsChanged = snapshot.state.hasVoxels();
                info = snapshot.state.info();
                slots_ = {StageSlot{{snapshot.stageName, "file"}, StageStatus::Done, 1.0f, std::move(snapshot)}};
            }

            const bool resized = !info_ || info_->width != info.width || info_->length != info.length;
            info_ = info;
            if (resized)
                resetCameras();
            configError_.clear();
            jobError_.clear();
            if (!job_)
                onSlotsChanged();
        } catch (const std::exception& e) {
            // Keep showing the previous result so a typo in the config is not disruptive.
            configError_ = e.what();
        }
        std::error_code ec;
        lastWrite_ = fs::last_write_time(source_, ec);
    }

    void watchSource() {
        if (!isConfig_ || GetTime() - lastWatchCheck_ < 0.5)
            return;
        lastWatchCheck_ = GetTime();
        std::error_code ec;
        const auto writeTime = fs::last_write_time(source_, ec);
        if (!ec && writeTime != lastWrite_)
            reload();
    }

    void syncJob() {
        if (job_ && job_->poll(slots_, jobError_))
            onSlotsChanged();
        std::erase_if(retiredJobs_, [](const auto& job) { return job->finished(); });
    }

    void onSlotsChanged() {
        if (slots_.empty()) {
            selected_ = 0;
            return;
        }
        selected_ = std::clamp(selected_, 0, static_cast<int>(slots_.size()) - 1);
        if (following_) {
            const auto running = std::find_if(slots_.begin(), slots_.end(), [](const StageSlot& s) {
                return s.status == StageStatus::Running || s.status == StageStatus::Failed;
            });
            int target = selected_;
            if (running != slots_.end())
                target = static_cast<int>(running - slots_.begin());
            else if (slots_.back().status == StageStatus::Done)
                target = static_cast<int>(slots_.size()) - 1;
            if (target != selected_) {
                selected_ = target;
                wantDefaultView_ = true;
            }
        }
        ensureView();
        pruneGpuCache();
    }

    // --- Selection -------------------------------------------------------------

    const wg::Snapshot* currentSnapshot() const {
        if (slots_.empty() || !slots_[selected_].snapshot)
            return nullptr;
        return &*slots_[selected_].snapshot;
    }

    void selectStage(int index) {
        if (slots_.empty())
            return;
        selected_ = std::clamp(index, 0, static_cast<int>(slots_.size()) - 1);
        const StageStatus status = slots_[selected_].status;
        following_ = status == StageStatus::Running ||
                     (selected_ == static_cast<int>(slots_.size()) - 1 && status == StageStatus::Done);
        wantDefaultView_ = true;
        ensureView();
    }

    // Picks what the stage produced when arriving on it (possibly later, once
    // its first preview arrives), or when the current view no longer exists.
    void ensureView() {
        const wg::Snapshot* snapshot = currentSnapshot();
        if (!snapshot)
            return;
        if (wantDefaultView_ || !wg::hasView(snapshot->state, view_)) {
            view_ = wg::defaultView(*snapshot);
            wantDefaultView_ = false;
        }
    }

    void cycleView() {
        const wg::Snapshot* snapshot = currentSnapshot();
        if (!snapshot)
            return;
        const auto views = wg::availableViews(snapshot->state);
        if (views.empty())
            return;
        auto it = std::find(views.begin(), views.end(), view_);
        view_ = (it == views.end() || ++it == views.end()) ? views.front() : *it;
    }

    // --- GPU resources -----------------------------------------------------------
    //
    // Cached per (data, mode, color map). Each entry keeps its data alive so the
    // address cannot be reused by another layer; entries whose data is no longer
    // referenced by any stage are dropped.

    using GpuKey = std::tuple<const void*, ViewMode, int>;
    struct GpuEntry {
        std::shared_ptr<const void> data;
        Texture2D texture{};
        Model model{};
    };

    const GpuEntry& gpuResource(const wg::WorldState& state) {
        const bool voxels = view_.kind == wg::View::Kind::Voxels;
        auto handle = voxels ? state.voxelsHandle() : state.layerHandle(view_.layer);
        const GpuKey key{handle.get(), mode_, voxels ? -1 : static_cast<int>(colorMap_)};
        lastDrawn_ = key;
        if (const auto it = gpuCache_.find(key); it != gpuCache_.end())
            return it->second;

        GpuEntry entry{std::move(handle)};
        if (mode_ == ViewMode::Map2D) {
            const auto image = wg::renderView(state, view_, colorMap_);
            const Image rlImage{const_cast<std::uint8_t*>(image.pixels.data()), image.width, image.height, 1,
                                PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            entry.texture = LoadTextureFromImage(rlImage);
            SetTextureFilter(entry.texture, TEXTURE_FILTER_POINT);
        } else if (voxels) {
            entry.model = buildVoxelModel(state.voxels());
        } else {
            const auto& grid = state.layer(view_.layer);
            const auto range = wg::valueRange(grid);
            const int sea = state.info().seaLevel;
            const bool terrain = colorMap_ == wg::ColorMap::Terrain;
            entry.model = buildHeightfieldModel(
                grid.width(), grid.length(),
                [&](int x, int z) { return terrain ? std::max(grid.at(x, z), static_cast<float>(sea)) : grid.at(x, z); },
                [&](int x, int z) { return wg::layerColor(grid.at(x, z), colorMap_, range, sea); });
        }
        return gpuCache_.emplace(key, std::move(entry)).first->second;
    }

    void unload(const GpuKey& key, GpuEntry& entry) {
        if (std::get<ViewMode>(key) == ViewMode::Map2D)
            UnloadTexture(entry.texture);
        else
            UnloadModel(entry.model);
    }

    void pruneGpuCache() {
        for (auto it = gpuCache_.begin(); it != gpuCache_.end();) {
            // Only the cache still references this data: no stage shows it anymore.
            // The last drawn entry stays, to keep something on screen while waiting.
            if (it->second.data.use_count() == 1 && it->first != lastDrawn_) {
                unload(it->first, it->second);
                it = gpuCache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clearGpuCache() {
        for (auto& [key, entry] : gpuCache_)
            unload(key, entry);
        gpuCache_.clear();
    }

    // --- Cameras & input -----------------------------------------------------------

    void resetCameras() {
        if (!info_)
            return;
        const float w = static_cast<float>(info_->width), l = static_cast<float>(info_->length);
        const float viewWidth = static_cast<float>(GetScreenWidth() - kPanelWidth - 2 * kMargin);

        camera2D_.target = {w * 0.5f, l * 0.5f};
        camera2D_.offset = {kPanelWidth + kMargin + viewWidth * 0.5f, GetScreenHeight() * 0.5f};
        camera2D_.rotation = 0.0f;
        camera2D_.zoom = 0.9f * std::min(viewWidth / w, GetScreenHeight() / l);

        orbitTarget_ = {w * 0.5f, static_cast<float>(info_->seaLevel), l * 0.5f};
        orbitYaw_ = 0.8f;
        orbitPitch_ = 0.6f;
        orbitDistance_ = std::max(w, l) * 1.1f;
        rlSetClipPlanes(1.0, std::max(4000.0, 6.0 * std::max(w, l)));
    }

    bool mouseOverPanel() const {
        const Vector2 m = GetMousePosition();
        return m.x < kPanelWidth + kMargin && m.y < panelHeight_ + kMargin;
    }

    void handleInput() {
        if (IsKeyPressed(KEY_R))
            reload();
        if (IsKeyPressed(KEY_TAB))
            mode_ = mode_ == ViewMode::Map2D ? ViewMode::Terrain3D : ViewMode::Map2D;
        if (IsKeyPressed(KEY_F))
            resetCameras();
        if (IsKeyPressed(KEY_L))
            cycleView();
        if (IsKeyPressed(KEY_C))
            colorMap_ = static_cast<wg::ColorMap>((static_cast<int>(colorMap_) + 1) % static_cast<int>(wg::ColorMap::Count));
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_RIGHT))
            selectStage(selected_ + 1);
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_LEFT))
            selectStage(selected_ - 1);
        for (int key = KEY_ONE; key <= KEY_NINE; ++key)
            if (IsKeyPressed(key) && key - KEY_ONE < static_cast<int>(slots_.size()))
                selectStage(key - KEY_ONE);

        // Clicking a stage in the panel; dragging only starts outside of it.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            dragging_ = !mouseOverPanel();
            const Vector2 m = GetMousePosition();
            const int row = static_cast<int>(std::floor((m.y - stageListTop_) / kLine));
            if (!dragging_ && m.y >= stageListTop_ && row < static_cast<int>(slots_.size()))
                selectStage(row);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            dragging_ = false;

        if (mode_ == ViewMode::Map2D)
            updateMapCamera();
        else
            updateOrbitCamera();
    }

    void updateMapCamera() {
        if (dragging_)
            camera2D_.target = Vector2Subtract(camera2D_.target, Vector2Scale(GetMouseDelta(), 1.0f / camera2D_.zoom));
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel()) {
            // Zoom around the cursor.
            const Vector2 mouse = GetMousePosition();
            camera2D_.target = GetScreenToWorld2D(mouse, camera2D_);
            camera2D_.offset = mouse;
            camera2D_.zoom = std::clamp(camera2D_.zoom * std::exp(wheel * 0.15f), 0.02f, 64.0f);
        }
    }

    void updateOrbitCamera() {
        const Vector2 delta = GetMouseDelta();
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            orbitYaw_ -= delta.x * 0.005f;
            orbitPitch_ = std::clamp(orbitPitch_ + delta.y * 0.005f, 0.05f, 1.5f);
        }
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            // Pan on the horizontal plane.
            const float speed = orbitDistance_ * 0.0015f;
            const Vector3 right{std::cos(orbitYaw_), 0.0f, -std::sin(orbitYaw_)};
            const Vector3 forward{-std::sin(orbitYaw_), 0.0f, -std::cos(orbitYaw_)};
            orbitTarget_ = Vector3Add(orbitTarget_, Vector3Scale(right, -delta.x * speed));
            orbitTarget_ = Vector3Add(orbitTarget_, Vector3Scale(forward, delta.y * speed));
        }
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel())
            orbitDistance_ = std::clamp(orbitDistance_ * std::exp(-wheel * 0.1f), 5.0f, 20000.0f);
    }

    Camera3D orbitCamera() const {
        Camera3D camera{};
        camera.target = orbitTarget_;
        camera.position = Vector3Add(orbitTarget_, {orbitDistance_ * std::cos(orbitPitch_) * std::sin(orbitYaw_),
                                                    orbitDistance_ * std::sin(orbitPitch_),
                                                    orbitDistance_ * std::cos(orbitPitch_) * std::cos(orbitYaw_)});
        camera.up = {0.0f, 1.0f, 0.0f};
        camera.fovy = 50.0f;
        camera.projection = CAMERA_PERSPECTIVE;
        return camera;
    }

    // --- Drawing ---------------------------------------------------------------------

    void drawWorld() {
        const GpuEntry* entry = nullptr;
        if (const wg::Snapshot* snapshot = currentSnapshot(); snapshot && wg::hasView(snapshot->state, view_)) {
            entry = &gpuResource(snapshot->state);
        } else if (const auto it = gpuCache_.find(lastDrawn_);
                   it != gpuCache_.end() && std::get<ViewMode>(lastDrawn_) == mode_) {
            entry = &it->second; // nothing to show yet: keep the previous image
        }
        if (!entry || !info_)
            return;

        if (mode_ == ViewMode::Map2D) {
            BeginMode2D(camera2D_);
            DrawTexture(entry->texture, 0, 0, WHITE);
            DrawRectangleLinesEx({0, 0, static_cast<float>(info_->width), static_cast<float>(info_->length)},
                                 2.0f / camera2D_.zoom, GRAY);
            EndMode2D();
        } else {
            BeginMode3D(orbitCamera());
            DrawModel(entry->model, {0, 0, 0}, 1.0f, WHITE);
            EndMode3D();
        }
    }

    void drawPanel() {
        const int x = kMargin + 10;
        int y = kMargin + 8;
        const auto text = [&](const std::string& s, Color color = kText) {
            DrawText(s.c_str(), x, y, kFont, color);
            y += kLine;
        };

        DrawRectangle(kMargin, kMargin, kPanelWidth, panelHeight_, kPanel);
        text(source_.filename().string() + (isConfig_ ? "  (live reload)" : ""));
        if (info_)
            text(TextFormat("%d x %d   y %d / %d   sea %d", info_->width, info_->length, info_->minY, info_->maxY,
                            info_->seaLevel),
                 kDimText);
        y += 6;

        text(slots_.empty() ? "Stages: none (empty pipeline)" : "Stages  (up/down, 1-9, click)", kDimText);
        stageListTop_ = static_cast<float>(y);
        for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
            drawStageRow(i, x, y);
        y += 6;

        if (const wg::Snapshot* snapshot = currentSnapshot()) {
            std::string views;
            for (const auto& v : wg::availableViews(snapshot->state))
                views += v == view_ ? "[" + v.label() + "] " : v.label() + " ";
            text("View (L): " + (views.empty() ? std::string("nothing yet") : views));
            if (view_.kind == wg::View::Kind::Layer)
                text("Colors (C): " + std::string(wg::colorMapName(colorMap_)));
            else if (mode_ == ViewMode::Terrain3D)
                text(TextFormat("1 cube = %d blocks (LOD)", voxelLodStep(snapshot->state.voxels())), kDimText);
            y += 6;
        }
        text(mode_ == ViewMode::Map2D ? "2D: drag pan, wheel zoom" : "3D: left orbit, right pan, wheel", kDimText);
        text("TAB 2D/3D   R regenerate   F reset view", kDimText);
        panelHeight_ = y - kMargin + 4;
    }

    void drawStageRow(int i, int x, int& y) {
        const StageSlot& slot = slots_[i];
        const int right = kMargin + kPanelWidth - 12;
        if (i == selected_)
            DrawRectangle(kMargin + 4, y - 2, kPanelWidth - 8, kLine, kHighlight);

        Color color = kText;
        std::string detail;
        switch (slot.status) {
        case StageStatus::Pending: color = kPendingText; break;
        case StageStatus::Running:
            detail = TextFormat("%d%%", static_cast<int>(slot.progress * 100.0f));
            DrawRectangle(kMargin + 4, y + kLine - 5, static_cast<int>((kPanelWidth - 8) * slot.progress), 3, kProgress);
            break;
        case StageStatus::Done: detail = TextFormat("%.0f ms", slot.snapshot->seconds * 1000.0); break;
        case StageStatus::Failed:
            color = kError;
            detail = "failed";
            break;
        }
        DrawText(TextFormat("%d. %s", i + 1, slot.description.name.c_str()), x, y, kFont, color);
        DrawText(detail.c_str(), right - MeasureText(detail.c_str(), kFont), y, kFont,
                 slot.status == StageStatus::Failed ? kError : kDimText);
        y += kLine;
    }

    void drawStatusBar() {
        std::string status;
        bool error = true;
        if (!configError_.empty())
            status = configError_;
        else if (!jobError_.empty())
            status = jobError_;
        else {
            error = false;
            if (job_ && !job_->finished())
                status = TextFormat("Generating... %.1f s", job_->elapsedSeconds());
            else if (job_)
                status = TextFormat("Generated in %.0f ms", job_->elapsedSeconds() * 1000.0);
        }

        const int h = kLine + 8;
        const int y = GetScreenHeight() - h;
        DrawRectangle(0, y, GetScreenWidth(), h, kPanel);
        const std::string hover = hoverInfo();
        DrawText(hover.c_str(), 12, y + 6, kFont, kText);
        const int statusX = std::max(12 + MeasureText(hover.c_str(), kFont) + 30,
                                     GetScreenWidth() - MeasureText(status.c_str(), kFont) - 12);
        DrawText(status.c_str(), statusX, y + 6, kFont, error ? kError : kDimText);
    }

    std::string hoverInfo() const {
        const wg::Snapshot* snapshot = currentSnapshot();
        if (mode_ != ViewMode::Map2D || !snapshot || !wg::hasView(snapshot->state, view_) || mouseOverPanel())
            return "";
        const auto& state = snapshot->state;
        const Vector2 p = GetScreenToWorld2D(GetMousePosition(), camera2D_);
        const int x = static_cast<int>(std::floor(p.x)), z = static_cast<int>(std::floor(p.y));
        if (x < 0 || z < 0 || x >= state.info().width || z >= state.info().length)
            return "";
        if (view_.kind == wg::View::Kind::Voxels) {
            const auto& voxels = state.voxels();
            const int top = voxels.topNonAir(x, z);
            return TextFormat("x %d  z %d   top y %d  %s", x, z, top,
                              std::string(wg::blockInfo(voxels.get(x, top, z)).name).c_str());
        }
        return TextFormat("x %d  z %d   %s = %.2f", x, z, view_.layer.c_str(), state.layer(view_.layer).at(x, z));
    }

    // --- State -----------------------------------------------------------------------

    fs::path source_;
    bool isConfig_;
    fs::file_time_type lastWrite_{};
    double lastWatchCheck_ = 0.0;

    std::unique_ptr<GenerationJob> job_;
    std::vector<std::unique_ptr<GenerationJob>> retiredJobs_; // cancelled, finishing in the background
    std::vector<StageSlot> slots_;
    std::optional<wg::WorldInfo> info_;
    std::string configError_;
    std::string jobError_;

    int selected_ = 0;
    bool following_ = true; // selection tracks the running stage
    bool wantDefaultView_ = true;
    wg::View view_;
    wg::ColorMap colorMap_ = wg::ColorMap::Terrain;

    std::map<GpuKey, GpuEntry> gpuCache_;
    GpuKey lastDrawn_{};

    ViewMode mode_ = ViewMode::Map2D;
    Camera2D camera2D_{};
    Vector3 orbitTarget_{};
    float orbitYaw_ = 0.0f;
    float orbitPitch_ = 0.0f;
    float orbitDistance_ = 100.0f;
    bool dragging_ = false;

    float stageListTop_ = 0.0f;
    int panelHeight_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: wview <config.json | world.wgw>\n");
        return 2;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 800, "World Generator Viewer");
    {
        Viewer viewer(argv[1]);
        viewer.run();
    }
    CloseWindow();
    return 0;
}
