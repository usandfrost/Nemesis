# Host tools for the content pipeline (analysis, cost, rendering). Build OUTSIDE the repo:
#   cmake -S content/tools -B <scratch>/build -DCMAKE_BUILD_TYPE=Release && cmake --build <scratch>/build -j8
# Reads the firmware sources, never writes to them; tests/build and firmware/build are not touched.
cmake_minimum_required(VERSION 3.20)
project(dboy_content CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(FW ${REPO}/firmware)
set(HYB ${REPO}/ref/upstream/sdk/dsp)
set(DBOY_GEN_DIR ${REPO}/content/data CACHE PATH "directory holding Presets_generated.inc")

# the CATHODE fork exactly as tests/CMakeLists.txt builds it (double, firmware optimisations on, 8-voice pool)
file(GLOB FW_DSP_SRCS CONFIGURE_DEPENDS "${FW}/dsp/src/*.cpp")
file(GLOB_RECURSE FW_DSP_PLAITS CONFIGURE_DEPENDS "${FW}/dsp/thirdparty/plaits/*.cc")
add_library(fw_dsp STATIC ${FW_DSP_SRCS} ${FW_DSP_PLAITS})
target_include_directories(fw_dsp PUBLIC "${FW}/dsp/include" "${FW}/dsp/thirdparty/plaits")
target_compile_definitions(fw_dsp PUBLIC NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1 NEMESIS_FORK_MAX_VOICES=8 DSP_IS_FORK=1)
set_source_files_properties(${FW_DSP_PLAITS} PROPERTIES COMPILE_OPTIONS -w)

set(APP_SRCS ${FW}/src/app/Model.cpp ${FW}/src/app/Params.cpp ${FW}/src/app/SampleLib.cpp ${FW}/src/app/CostBridge.cpp
    ${FW}/src/app/Ui.cpp ${FW}/src/app/Sequencer.cpp ${FW}/src/app/ProjectFile.cpp ${FW}/src/app/MidiMap.cpp ${FW}/src/app/FxParams.cpp ${FW}/src/app/WaveLib.cpp)

# panel facts and the cost model, no audio: ranges, per-voice cost, sanitiser check
add_executable(panel_tool panel_tool.cpp ${APP_SRCS})
target_include_directories(panel_tool PRIVATE ${FW}/src)
target_compile_options(panel_tool PRIVATE -Wall -Wno-unused-parameter)

# the instrument with the wavetable prototype of PatchMap (shadow/audio/PatchMap.h comes first, so Instrument.cpp sees it)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/render_presets.cpp)
add_executable(render_presets render_presets.cpp PatchMap_wt.cpp ${APP_SRCS} ${FW}/src/audio/Sampler.cpp ${FW}/src/audio/Instrument.cpp)
target_include_directories(render_presets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shadow ${FW}/src ${DBOY_GEN_DIR})
target_link_libraries(render_presets PRIVATE fw_dsp)
target_compile_options(render_presets PRIVATE -Wall -Wno-unused-parameter)
endif()
