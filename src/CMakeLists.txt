add_library(wg
  wg/core/Block.cpp
  wg/core/Config.cpp
  wg/core/Parallel.cpp
  wg/core/VoxelVolume.cpp
  wg/core/WorldState.cpp
  wg/gen/Pipeline.cpp
  wg/gen/StageParams.cpp
  wg/gen/StageRegistry.cpp
  wg/gen/stages/BuiltinStages.cpp
  wg/gen/stages/IslandInitStage.cpp
  wg/gen/stages/ParametersStage.cpp
  wg/gen/stages/SimulationStage.cpp
  wg/params/IslandParams.cpp
  wg/params/ParamResolver.cpp
  wg/island/Geology.cpp
  wg/island/UpliftPattern.cpp
  wg/sim/Climate.cpp
  wg/sim/Coastal.cpp
  wg/sim/Fluvial.cpp
  wg/sim/Glacier.cpp
  wg/sim/Hillslope.cpp
  wg/sim/Marine.cpp
  wg/sim/Routing.cpp
  wg/sim/TimeParams.cpp
  wg/io/PngExport.cpp
  wg/io/WorldFile.cpp
  wg/render/CrossSection.cpp
  wg/render/ImageOps.cpp
  wg/render/MapRenderer.cpp
)
find_package(Threads REQUIRED)
target_include_directories(wg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wg
  PUBLIC nlohmann_json::nlohmann_json fastnoiselite Threads::Threads
  PRIVATE stb)
target_compile_options(wg PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)
