add_library(wg
  wg/core/Block.cpp
  wg/core/Config.cpp
  wg/core/VoxelVolume.cpp
  wg/core/WorldState.cpp
  wg/noise/Perlin.cpp
  wg/gen/Pipeline.cpp
  wg/gen/StageParams.cpp
  wg/gen/StageRegistry.cpp
  wg/gen/stages/BuiltinStages.cpp
  wg/gen/stages/FlatStage.cpp
  wg/gen/stages/IslandMaskStage.cpp
  wg/gen/stages/PerlinStage.cpp
  wg/gen/stages/VoxelizeStage.cpp
  wg/io/PngExport.cpp
  wg/io/WorldFile.cpp
  wg/render/MapRenderer.cpp
)
target_include_directories(wg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wg
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE stb)
target_compile_options(wg PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)
