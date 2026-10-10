include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# JSON parsing (world config)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# stb (header-only): PNG writing
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# FastNoiseLite (header-only): OpenSimplex2 / fBm noise
FetchContent_Declare(fastnoiselite
  URL https://github.com/Auburn/FastNoiseLite/archive/refs/tags/v1.1.1.tar.gz
  URL_HASH SHA256=8b1d439af1ae88eaf8d3d0838540796dfc92ddd216230b2816f0c5bcbb3c9824
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(fastnoiselite)
add_library(fastnoiselite INTERFACE)
target_include_directories(fastnoiselite SYSTEM INTERFACE ${fastnoiselite_SOURCE_DIR}/Cpp)

# raylib: windowing + rendering for the viewer
if(WG_BUILD_VIEWER)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(raylib)
endif()
