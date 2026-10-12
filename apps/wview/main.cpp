// wview: interactive, live preview of a world generation, stage by stage.
//
//   wview <config.jsonc | world.wgw>
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
#include "wg/render/CrossSection.hpp"
#include "wg/island/IslandLayers.hpp"
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
// Parameter sources in the parameters view.
constexpr Color kFromSeed{120, 210, 140, 255};
constexpr Color kFromConfig{235, 235, 235, 255};
constexpr Color kFromDefault{130, 130, 135, 255};

constexpr int kFont = 16;
constexpr int kLine = 22;
constexpr int kPanelWidth = 330;
constexpr int kMargin = 8;

// How often a running stage sends a live preview (each one is re-rendered).
constexpr double kPreviewInterval = 0.1;

// Playback speeds of iterative stages (erosion), in steps per frame; 0 = as
// fast as possible.
constexpr int kSpeeds[] = {1, 2, 4, 8, 16, 32, 64, 0};
constexpr int kSpeedCount = static_cast<int>(sizeof kSpeeds / sizeof kSpeeds[0]);

// UI font, loaded in main() (raylib's default bitmap font if the file is missing).
Font gFont{};

void drawText(const char* text, int x, int y, int size, Color color) {
    DrawTextEx(gFont, text, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size), 0.0f, color);
}

int measureText(const char* text, int size) {
    return static_cast<int>(MeasureTextEx(gFont, text, static_cast<float>(size), 0.0f).x);
}

enum class ViewMode { Map2D, Terrain3D };

class Viewer {
public:
    explicit Viewer(fs::path source)
        : source_(std::move(source)), isConfig_(source_.extension() == ".json" || source_.extension() == ".jsonc") {}

    ~Viewer() {
        job_.reset();
        retiredJobs_.clear();
        clearGpuCache();
        if (sectionTexture_.id != 0)
            UnloadTexture(sectionTexture_);
    }

    void run() {
        reload();
        while (!WindowShouldClose()) {
            if (job_)
                job_->nextFrame();
            syncJob();
            handleInput();
            watchSource();

            BeginDrawing();
            ClearBackground(kBackground);
            drawWorld();
            if (sectionVisible_)
                drawSection();
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
                config_ = std::make_shared<const wg::WorldConfig>(config);
                // The previous job may still be running a stage that never reports
                // progress: let it finish in the background instead of blocking.
                if (job_) {
                    job_->cancel();
                    retiredJobs_.push_back(std::move(job_));
                }
                job_ = std::make_unique<GenerationJob>(std::move(pipeline), config_, kPreviewInterval);
                job_->setPlayback(paused_, kSpeeds[speed_]);
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
            // Relief from the layer itself if it is a height, otherwise from "h" (if
            // on the same grid), draped with the colors of the displayed layer.
            const auto& grid = state.layer(view_.layer);
            const wg::LayerPalette palette(grid, state.layerMeta(view_.layer), colorMap_, state.info().seaLevelM());
            const wg::Grid2D<float>* relief = nullptr;
            if (state.layerMeta(view_.layer).style == wg::LayerStyle::Height)
                relief = &grid;
            else if (state.hasLayer(wg::layers::kHeight) && state.layer(wg::layers::kHeight).width() == grid.width() &&
                     state.layer(wg::layers::kHeight).length() == grid.length())
                relief = &state.layer(wg::layers::kHeight);
            const auto sea = static_cast<float>(state.info().seaLevelM());
            const auto metersToBlocks = static_cast<float>(1.0 / state.info().blockSizeM);
            entry.model = buildHeightfieldModel(
                grid.width(), grid.length(),
                [&](int x, int z) { return (relief ? std::max(relief->at(x, z), sea) : sea) * metersToBlocks; },
                [&](int x, int z) { return palette.color(grid.at(x, z)); }, 512,
                static_cast<double>(state.info().width) / grid.width());
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

    // --- Cross-section --------------------------------------------------------
    //
    // A vertical cut of the rock in place along a line across the map, shown in
    // a panel at the bottom: X toggles it, S puts the line's center under the
    // mouse, Q / E rotate it, the wheel over the panel sets the exaggeration.

    Rectangle sectionRect() const {
        const float x = kPanelWidth + 2.0f * kMargin;
        const float top = GetScreenHeight() * 0.6f;
        const float bottom = static_cast<float>(GetScreenHeight() - (kLine + 8) - kMargin);
        return {x, top, std::max(1.0f, GetScreenWidth() - x - kMargin), std::max(1.0f, bottom - top)};
    }

    bool mouseOverSection() const {
        return sectionVisible_ && CheckCollisionPointRec(GetMousePosition(), sectionRect());
    }

    // Recomputes the section image when the data, the line or the panel size changed.
    void updateSection() {
        const wg::Snapshot* snapshot = currentSnapshot();
        if (!snapshot || !wg::canRenderCrossSection(snapshot->state))
            return;
        const Rectangle rect = sectionRect();
        const auto& state = snapshot->state;
        if (fitSection_) { // frame the relief when the section is opened or moved
            section_.exaggeration = std::clamp(
                wg::fitCrossSectionExaggeration(state, section_, static_cast<int>(rect.width), static_cast<int>(rect.height)),
                0.25, 50.0);
            fitSection_ = false;
        }
        const SectionKey key{state.layerHandle(wg::layers::kHeight), state.layerHandle(wg::layers::kHeightInitial),
                             state.paramsHandle(), section_.x, section_.y, section_.angleDeg, section_.exaggeration,
                             static_cast<int>(rect.width), static_cast<int>(rect.height)};
        if (sectionImage_ && key == sectionKey_)
            return;
        sectionKey_ = key;
        sectionImage_ = wg::renderCrossSection(state, section_, key.width, key.height);
        if (sectionTexture_.id != 0)
            UnloadTexture(sectionTexture_);
        const Image image{sectionImage_->image.pixels.data(), sectionImage_->image.width, sectionImage_->image.height, 1,
                          PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        sectionTexture_ = LoadTextureFromImage(image);
    }

    void handleSectionInput() {
        if (IsKeyPressed(KEY_X)) {
            sectionVisible_ = !sectionVisible_;
            fitSection_ = sectionVisible_;
        }
        if (!sectionVisible_ || !info_)
            return;
        if (IsKeyDown(KEY_Q))
            section_.angleDeg = std::fmod(section_.angleDeg - 60.0 * GetFrameTime() + 360.0, 360.0);
        if (IsKeyDown(KEY_E))
            section_.angleDeg = std::fmod(section_.angleDeg + 60.0 * GetFrameTime(), 360.0);
        if (IsKeyPressed(KEY_S) && mode_ == ViewMode::Map2D && !mouseOverPanel() && !mouseOverSection()) {
            const Vector2 p = GetScreenToWorld2D(GetMousePosition(), camera2D_);
            section_.x = p.x * info_->blockSizeM - info_->widthM() / 2.0;
            section_.y = p.y * info_->blockSizeM - info_->lengthM() / 2.0;
            fitSection_ = true;
        }
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && mouseOverSection())
            section_.exaggeration = std::clamp(section_.exaggeration * std::exp(wheel * 0.15), 0.25, 50.0);
    }

    void drawSection() {
        updateSection();
        const Rectangle rect = sectionRect();
        DrawRectangleRec(rect, kBackground); // opaque: the map must not show through the air
        DrawRectangleLinesEx(rect, 1.0f, kDimText);
        if (!sectionImage_ || sectionTexture_.id == 0) {
            drawText("Section: nothing to cut yet", static_cast<int>(rect.x) + 10, static_cast<int>(rect.y) + 8, kFont,
                     kDimText);
            return;
        }
        const auto& cut = *sectionImage_;
        DrawTexture(sectionTexture_, static_cast<int>(rect.x), static_cast<int>(rect.y), WHITE);

        // Sea level, altitude scale and header.
        const double sea = info_->seaLevelM();
        const float seaY = rect.y + static_cast<float>((cut.zTop - sea) / cut.metersPerPixelZ);
        if (seaY > rect.y && seaY < rect.y + rect.height)
            DrawLineEx({rect.x, seaY}, {rect.x + rect.width, seaY}, 1.0f, {120, 160, 255, 160});
        const int x = static_cast<int>(rect.x) + 8;
        drawText(TextFormat("Section  %.0f deg  x%.1f vertical   (S place, Q/E rotate, wheel exaggeration, X hide)",
                            section_.angleDeg, section_.exaggeration),
                 x, static_cast<int>(rect.y) + 6, kFont, kText);
        drawText(TextFormat("%.0f m", cut.zTop), x, static_cast<int>(rect.y) + 6 + kLine, kFont, kDimText);
        drawText(TextFormat("%.0f m", cut.zTop - rect.height * cut.metersPerPixelZ), x,
                 static_cast<int>(rect.y + rect.height) - kLine, kFont, kDimText);

        // Legend: the materials that appear in the cut.
        std::vector<bool> present(cut.materialNames.size(), false);
        for (std::int16_t id : cut.materials)
            if (id >= 0)
                present[id] = true;
        int ly = static_cast<int>(rect.y) + 6 + kLine;
        const int lx = static_cast<int>(rect.x + rect.width) - 150;
        for (std::size_t id = 0; id < present.size(); ++id) {
            if (!present[id])
                continue;
            const wg::Rgb c = wg::materialColor(cut.materialNames[id]);
            DrawRectangle(lx, ly + 3, 12, 12, {c.r, c.g, c.b, 255});
            drawText(cut.materialNames[id].c_str(), lx + 18, ly, kFont, kText);
            ly += kLine - 2;
        }
    }

    // The section line on the map (2D), in block coordinates.
    void drawSectionLine() const {
        if (!sectionVisible_ || !sectionImage_ || !info_)
            return;
        const auto toBlocks = [&](double x, double y) {
            return Vector2{static_cast<float>((x + info_->widthM() / 2.0) / info_->blockSizeM),
                           static_cast<float>((y + info_->lengthM() / 2.0) / info_->blockSizeM)};
        };
        const float thickness = 2.5f / camera2D_.zoom;
        DrawLineEx(toBlocks(sectionImage_->startX, sectionImage_->startY),
                   toBlocks(sectionImage_->endX, sectionImage_->endY), thickness, YELLOW);
        DrawCircleV(toBlocks(section_.x, section_.y), 4.0f / camera2D_.zoom, YELLOW);
        DrawCircleV(toBlocks(sectionImage_->startX, sectionImage_->startY), 5.0f / camera2D_.zoom, ORANGE);
    }

    std::string sectionHoverInfo() const {
        const auto& cut = *sectionImage_;
        const Rectangle rect = sectionRect();
        const Vector2 m = GetMousePosition();
        const int c = static_cast<int>(m.x - rect.x), r = static_cast<int>(m.y - rect.y);
        if (c < 0 || r < 0 || c >= cut.image.width || r >= cut.image.height)
            return "";
        const double distance = (c + 0.5) * cut.metersPerPixelX;
        const double z = cut.zTop - (r + 0.5) * cut.metersPerPixelZ;
        const std::int16_t id = cut.materials[static_cast<std::size_t>(r) * cut.image.width + c];
        return TextFormat("section  %.0f m from the orange end   z %.1f m   %s", distance, z,
                          id >= 0 ? cut.materialNames[id].c_str() : (z <= info_->seaLevelM() ? "water" : "air"));
    }

    bool mouseOverPanel() const {
        const Vector2 m = GetMousePosition();
        return m.x < kPanelWidth + kMargin && m.y < panelHeight_ + kMargin;
    }

    void handleInput() {
        if (IsKeyPressed(KEY_R))
            reload();
        handlePlaybackInput();
        handleSectionInput();
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
            dragging_ = !mouseOverPanel() && !mouseOverSection();
            const Vector2 m = GetMousePosition();
            const int row = static_cast<int>(std::floor((m.y - stageListTop_) / kLine));
            if (!dragging_ && m.y >= stageListTop_ && row < static_cast<int>(slots_.size()))
                selectStage(row);
            const int layerRow = static_cast<int>(std::floor((m.y - layerListTop_) / kLine));
            if (!dragging_ && m.y >= layerListTop_ && layerRow < static_cast<int>(listedViews_.size()))
                view_ = listedViews_[layerRow];
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            dragging_ = false;

        if (view_.kind == wg::View::Kind::Parameters) {
            if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel() && !mouseOverSection())
                paramsScroll_ = std::max(0.0f, paramsScroll_ - wheel * 3.0f * kLine);
        } else if (mode_ == ViewMode::Map2D) {
            updateMapCamera();
        } else {
            updateOrbitCamera();
        }
    }

    void updateMapCamera() {
        if (dragging_)
            camera2D_.target = Vector2Subtract(camera2D_.target, Vector2Scale(GetMouseDelta(), 1.0f / camera2D_.zoom));
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel() && !mouseOverSection()) {
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
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel() && !mouseOverSection())
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
        if (const wg::Snapshot* snapshot = currentSnapshot();
            snapshot && view_.kind == wg::View::Kind::Parameters && snapshot->state.hasParams()) {
            drawParameters(snapshot->state.params());
            return;
        }
        const GpuEntry* entry = nullptr;
        if (const wg::Snapshot* snapshot = currentSnapshot(); snapshot && wg::hasView(snapshot->state, view_)) {
            entry = &gpuResource(snapshot->state); // image views only, parameters are handled above
        } else if (const auto it = gpuCache_.find(lastDrawn_);
                   it != gpuCache_.end() && std::get<ViewMode>(lastDrawn_) == mode_) {
            entry = &it->second; // nothing to show yet: keep the previous image
        }
        if (!entry || !info_)
            return;

        if (mode_ == ViewMode::Map2D) {
            BeginMode2D(camera2D_);
            // Layers can be coarser than the block grid: stretch them over the whole map.
            DrawTexturePro(entry->texture,
                           {0, 0, static_cast<float>(entry->texture.width), static_cast<float>(entry->texture.height)},
                           {0, 0, static_cast<float>(info_->width), static_cast<float>(info_->length)}, {0, 0}, 0.0f,
                           WHITE);
            DrawRectangleLinesEx({0, 0, static_cast<float>(info_->width), static_cast<float>(info_->length)},
                                 2.0f / camera2D_.zoom, GRAY);
            drawSectionLine();
            EndMode2D();
        } else {
            BeginMode3D(orbitCamera());
            DrawModel(entry->model, {0, 0, 0}, 1.0f, WHITE);
            EndMode3D();
        }
    }

    // Every resolved parameter, grouped by section, colored by where its value
    // comes from. Scrolled with the mouse wheel.
    void drawParameters(const wg::ResolvedParams& params) {
        const int left = kPanelWidth + 2 * kMargin + 16;
        const int valueX = left + 400;
        const int top = kMargin + 8;
        const int bottom = GetScreenHeight() - (kLine + 8);

        int counts[3] = {};
        for (const auto& entry : params.entries)
            ++counts[static_cast<int>(entry.source)];
        int x = left;
        const auto legend = [&](const char* label, Color color) {
            drawText(label, x, top, kFont, color);
            x += measureText(label, kFont) + 24;
        };
        legend(TextFormat("seed: %d", counts[0]), kFromSeed);
        legend(TextFormat("config: %d", counts[1]), kFromConfig);
        legend(TextFormat("default: %d", counts[2]), kFromDefault);
        legend("(wheel to scroll)", kDimText);

        const int listTop = top + kLine + 8;
        const int contentHeight = static_cast<int>(params.entries.size()) * kLine * 12 / 10;
        paramsScroll_ = std::min(paramsScroll_, static_cast<float>(std::max(0, contentHeight - (bottom - listTop))));

        BeginScissorMode(left - 4, listTop, GetScreenWidth() - left, bottom - listTop);
        int y = listTop - static_cast<int>(paramsScroll_);
        std::string section;
        for (const auto& entry : params.entries) {
            const std::string entrySection = entry.path.substr(0, entry.path.find_first_of(".["));
            if (entrySection != section) {
                section = entrySection;
                y += kLine / 5;
                if (y > listTop - kLine && y < bottom)
                    drawText(section.c_str(), left, y, kFont, kDimText);
                if (entry.path == section) { // top-level scalar such as "seed"
                    drawParamValue(entry, valueX, y);
                    y += kLine;
                    continue;
                }
                y += kLine;
            }
            if (y > listTop - kLine && y < bottom) {
                const Color color = entry.source == wg::ParamSource::Seed     ? kFromSeed
                                    : entry.source == wg::ParamSource::Config ? kFromConfig
                                                                              : kFromDefault;
                drawText(entry.path.substr(section.size() + (entry.path[section.size()] == '.')).c_str(), left + 20, y,
                         kFont, color);
                drawParamValue(entry, valueX, y);
            }
            y += kLine;
        }
        EndScissorMode();
    }

    // Compact JSON with short numbers.
    static std::string formatValue(const nlohmann::json& value) {
        if (value.is_number_float())
            return TextFormat("%.6g", value.get<double>());
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_array() || value.is_object()) {
            std::string text = value.is_array() ? "[" : "{";
            bool first = true;
            for (const auto& [key, item] : value.items()) {
                text += (first ? "" : ", ") + (value.is_object() ? key + ": " : "") + formatValue(item);
                first = false;
            }
            return text + (value.is_array() ? "]" : "}");
        }
        return value.dump();
    }

    static void drawParamValue(const wg::ParamEntry& entry, int x, int y) {
        std::string text = formatValue(entry.value);
        if (text.size() > 90)
            text = text.substr(0, 87) + "...";
        const Color color = entry.source == wg::ParamSource::Seed     ? kFromSeed
                            : entry.source == wg::ParamSource::Config ? kFromConfig
                                                                      : kFromDefault;
        drawText(text.c_str(), x, y, kFont, color);
    }

    void drawPanel() {
        const int x = kMargin + 10;
        int y = kMargin + 8;
        const auto text = [&](const std::string& s, Color color = kText) {
            drawText(s.c_str(), x, y, kFont, color);
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

        listedViews_.clear();
        layerListTop_ = 1e9f;
        if (const wg::Snapshot* snapshot = currentSnapshot()) {
            listedViews_ = wg::availableViews(snapshot->state);
            text(listedViews_.empty() ? "Layers: nothing yet" : "Layers  (L, click)", kDimText);
            layerListTop_ = static_cast<float>(y);
            for (const auto& v : listedViews_) {
                if (v == view_)
                    DrawRectangle(kMargin + 4, y - 2, kPanelWidth - 8, kLine, kHighlight);
                text(v.label());
            }
            y += 6;

            if (view_.kind == wg::View::Kind::Layer && wg::hasView(snapshot->state, view_)) {
                text("Colors (C): " + std::string(wg::colorMapName(colorMap_)));
                drawLegend(snapshot->state, x, y);
            } else if (view_.kind == wg::View::Kind::Voxels && mode_ == ViewMode::Terrain3D) {
                text(TextFormat("1 cube = %d blocks (LOD)", voxelLodStep(snapshot->state.voxels())), kDimText);
            }
            y += 6;
        }
        text(mode_ == ViewMode::Map2D ? "2D: drag pan, wheel zoom" : "3D: left orbit, right pan, wheel", kDimText);
        text("TAB 2D/3D  R regenerate  F reset", kDimText);
        text("X section  S place  Q/E rotate", kDimText);
        panelHeight_ = y - kMargin + 4;
    }

    // Category swatches of a categorical layer, or the value range of the others.
    void drawLegend(const wg::WorldState& state, int x, int& y) const {
        const auto& grid = state.layer(view_.layer);
        const auto& meta = state.layerMeta(view_.layer);
        const wg::LayerPalette palette(grid, meta, colorMap_, state.info().seaLevelM());
        if (meta.style == wg::LayerStyle::Categorical && colorMap_ == wg::ColorMap::Auto) {
            for (std::size_t id = 0; id < meta.categories.size(); ++id) {
                const wg::Rgb c = palette.color(static_cast<float>(id));
                DrawRectangle(x, y + 3, 12, 12, {c.r, c.g, c.b, 255});
                drawText(meta.categories[id].c_str(), x + 20, y, kFont, kDimText);
                y += kLine;
            }
            return;
        }
        const auto range = wg::valueRange(grid);
        drawText(TextFormat("min %.4g   max %.4g", range.min, range.max), x, y, kFont, kDimText);
        y += kLine;
    }

    // Space: play / pause, N: one step while paused, + / -: steps per frame.
    void handlePlaybackInput() {
        bool changed = false;
        if (IsKeyPressed(KEY_SPACE))
            paused_ = !paused_, changed = true;
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
            speed_ = std::min(speed_ + 1, kSpeedCount - 1), changed = true;
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
            speed_ = std::max(speed_ - 1, 0), changed = true;
        if (changed && job_)
            job_->setPlayback(paused_, kSpeeds[speed_]);
        if (IsKeyPressed(KEY_N) && job_)
            job_->stepOnce();
    }

    std::string playbackText() const {
        if (paused_)
            return "paused (space: play, N: step)";
        return kSpeeds[speed_] == 0 ? "playing, max speed" : TextFormat("playing, %d steps/frame", kSpeeds[speed_]);
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
            detail = slot.snapshot && slot.snapshot->step >= 0
                         ? TextFormat("step %d/%d", slot.snapshot->step, slot.snapshot->totalSteps)
                         : TextFormat("%d%%", static_cast<int>(slot.progress * 100.0f));
            DrawRectangle(kMargin + 4, y + kLine - 5, static_cast<int>((kPanelWidth - 8) * slot.progress), 3, kProgress);
            break;
        case StageStatus::Done: detail = TextFormat("%.0f ms", slot.snapshot->seconds * 1000.0); break;
        case StageStatus::Failed:
            color = kError;
            detail = "failed";
            break;
        }
        drawText(TextFormat("%d. %s", i + 1, slot.description.name.c_str()), x, y, kFont, color);
        drawText(detail.c_str(), right - measureText(detail.c_str(), kFont), y, kFont,
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
                status = std::string(TextFormat("Generating... %.1f s   ", job_->elapsedSeconds())) + playbackText();
            else if (job_)
                status = TextFormat("Generated in %.0f ms", job_->elapsedSeconds() * 1000.0);
        }

        const int h = kLine + 8;
        const int y = GetScreenHeight() - h;
        DrawRectangle(0, y, GetScreenWidth(), h, kPanel);
        const std::string hover = hoverInfo();
        drawText(hover.c_str(), 12, y + 6, kFont, kText);
        const int statusX = std::max(12 + measureText(hover.c_str(), kFont) + 30,
                                     GetScreenWidth() - measureText(status.c_str(), kFont) - 12);
        drawText(status.c_str(), statusX, y + 6, kFont, error ? kError : kDimText);
    }

    std::string hoverInfo() const {
        if (mouseOverSection())
            return sectionImage_ ? sectionHoverInfo() : "";
        const wg::Snapshot* snapshot = currentSnapshot();
        if (mode_ != ViewMode::Map2D || !snapshot || !view_.isImage() || !wg::hasView(snapshot->state, view_) ||
            mouseOverPanel())
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
        // Layers may be coarser than the block grid.
        const auto& grid = state.layer(view_.layer);
        const int i = x * grid.width() / state.info().width, j = z * grid.length() / state.info().length;
        const float value = grid.at(i, j);
        const auto& meta = state.layerMeta(view_.layer);
        const auto id = static_cast<std::size_t>(std::max(0.0f, std::round(value)));
        if (meta.style == wg::LayerStyle::Categorical && id < meta.categories.size())
            return TextFormat("x %d  z %d  cell %d,%d   %s = %s", x, z, i, j, view_.layer.c_str(),
                              meta.categories[id].c_str());
        return TextFormat("x %d  z %d  cell %d,%d   %s = %.4g", x, z, i, j, view_.layer.c_str(), value);
    }

    // --- State -----------------------------------------------------------------------

    fs::path source_;
    bool isConfig_;
    fs::file_time_type lastWrite_{};
    double lastWatchCheck_ = 0.0;

    std::shared_ptr<const wg::WorldConfig> config_;
    std::unique_ptr<GenerationJob> job_;
    std::vector<std::unique_ptr<GenerationJob>> retiredJobs_; // cancelled, finishing in the background
    std::vector<StageSlot> slots_;
    std::optional<wg::WorldInfo> info_;
    std::string configError_;
    std::string jobError_;

    int selected_ = 0;
    bool following_ = true; // selection tracks the running stage
    bool wantDefaultView_ = true;
    bool paused_ = false; // playback of iterative stages, kept across reloads
    int speed_ = kSpeedCount - 1;
    wg::View view_;
    wg::ColorMap colorMap_ = wg::ColorMap::Auto;

    std::map<GpuKey, GpuEntry> gpuCache_;
    GpuKey lastDrawn_{};

    ViewMode mode_ = ViewMode::Map2D;
    Camera2D camera2D_{};
    Vector3 orbitTarget_{};
    float orbitYaw_ = 0.0f;
    float orbitPitch_ = 0.0f;
    float orbitDistance_ = 100.0f;
    bool dragging_ = false;

    float paramsScroll_ = 0.0f;

    struct SectionKey {
        std::shared_ptr<const void> heightLayer, initialHeightLayer, params; // kept alive: pointers stay unique
        double x, y, angle, exaggeration;
        int width, height;
        bool operator==(const SectionKey&) const = default;
    };
    bool sectionVisible_ = false;
    bool fitSection_ = false;
    wg::CrossSection section_{};
    SectionKey sectionKey_{};
    std::optional<wg::CrossSectionImage> sectionImage_;
    Texture2D sectionTexture_{};
    float stageListTop_ = 0.0f;
    float layerListTop_ = 1e9f;
    std::vector<wg::View> listedViews_; // rows of the layer list, for clicks
    int panelHeight_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: wview <config.jsonc | world.wgw>\n");
        return 2;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 800, "World Generator Viewer");

    // Rasterized at the screen's pixel density so text stays sharp on HiDPI displays.
    std::vector<int> codepoints;
    for (int c = 32; c < 256; ++c)
        codepoints.push_back(c);
    const int fontPixels = static_cast<int>(std::lround(kFont * std::max(1.0f, GetWindowScaleDPI().x)));
    gFont = FileExists(WG_FONT_PATH) ? LoadFontEx(WG_FONT_PATH, fontPixels, codepoints.data(), static_cast<int>(codepoints.size()))
                                     : GetFontDefault();
    SetTextureFilter(gFont.texture, TEXTURE_FILTER_BILINEAR);
    {
        Viewer viewer(argv[1]);
        viewer.run();
    }
    if (gFont.texture.id != GetFontDefault().texture.id)
        UnloadFont(gFont);
    CloseWindow();
    return 0;
}
