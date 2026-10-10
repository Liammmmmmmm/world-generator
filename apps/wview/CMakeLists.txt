add_executable(wview main.cpp GenerationJob.cpp Meshes.cpp)
target_link_libraries(wview PRIVATE wg raylib)

# UI font (JetBrains Mono, OFL), downloaded once at configure time.
set(WG_FONT_PATH ${CMAKE_BINARY_DIR}/fonts/JetBrainsMono-Regular.ttf)
file(DOWNLOAD
  https://github.com/JetBrains/JetBrainsMono/raw/v2.304/fonts/ttf/JetBrainsMono-Regular.ttf
  ${WG_FONT_PATH}
  EXPECTED_HASH SHA256=a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f)
target_compile_definitions(wview PRIVATE WG_FONT_PATH="${WG_FONT_PATH}")
