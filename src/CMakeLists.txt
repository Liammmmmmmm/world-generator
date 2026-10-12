add_library(wg
  wg/core/Block.cpp
  wg/core/Config.cpp
  wg/core/VoxelVolume.cpp
  wg/core/WorldState.cpp
  wg/gen/Pipeline.cpp
  wg/gen/StageParams.cpp
  wg/gen/StageRegistry.cpp
  wg/gen/stages/BuiltinStages.cpp
  wg/gen/stages/ErosionStage.cpp
  wg/gen/stages/IslandInitStage.cpp
  wg/gen/stages/ParametersStage.cpp
  wg/params/IslandParams.cpp
  wg/params/ParamResolver.cpp
  wg/erosion/Drop.cpp
  wg/erosion/ParticleErosion.cpp
  wg/island/Geology.cpp
  wg/island/Relief.cpp
  wg/io/PngExport.cpp
  wg/io/WorldFile.cpp
  wg/render/CrossSection.cpp
  wg/render/ImageOps.cpp
  wg/render/MapRenderer.cpp
)
target_include_directories(wg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wg
  PUBLIC nlohmann_json::nlohmann_json fastnoiselite
  PRIVATE stb)
target_compile_options(wg PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)
