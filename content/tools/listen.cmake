# Host renderer of the second listening page (render_listen). run_listen2.sh copies this to a scratch CMakeLists.txt and fixes the REPO line.
cmake_minimum_required(VERSION 3.20)
project(dboy_listen2 CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FW ${REPO}/firmware)
file(GLOB FW_DSP_SRCS CONFIGURE_DEPENDS "${FW}/dsp/src/*.cpp")
file(GLOB_RECURSE FW_DSP_PLAITS CONFIGURE_DEPENDS "${FW}/dsp/thirdparty/plaits/*.cc")
add_library(fw_dsp STATIC ${FW_DSP_SRCS} ${FW_DSP_PLAITS})
target_include_directories(fw_dsp PUBLIC "${FW}/dsp/include" "${FW}/dsp/thirdparty/plaits")
target_compile_definitions(fw_dsp PUBLIC NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1 NEMESIS_FORK_MAX_VOICES=8 DSP_IS_FORK=1)
set_source_files_properties(${FW_DSP_PLAITS} PROPERTIES COMPILE_OPTIONS -w)
set(A ${FW}/src/app)
add_executable(render_listen ${REPO}/content/tools/render_listen.cpp
    ${A}/Model.cpp ${A}/Params.cpp ${A}/SampleLib.cpp ${A}/CostBridge.cpp ${A}/Ui.cpp ${A}/Sequencer.cpp ${A}/ProjectFile.cpp ${A}/MidiMap.cpp ${A}/FxParams.cpp ${A}/WaveLib.cpp
    ${FW}/src/audio/PatchMap.cpp ${FW}/src/audio/WaveStore.cpp ${FW}/src/audio/Sampler.cpp ${FW}/src/audio/Instrument.cpp
    ${FW}/src/fx/FxDelay.cpp ${FW}/src/fx/FxReverb.cpp ${FW}/src/fx/FxChorus.cpp ${FW}/src/fx/FxComp.cpp ${FW}/src/fx/FxAmp.cpp ${FW}/src/fx/FxSection.cpp)
target_include_directories(render_listen PRIVATE ${FW}/src ${REPO}/content/tools ${REPO}/tests/app)
target_compile_definitions(render_listen PRIVATE DBOY_PRESET_DIR="${REPO}/content/presets")   # the library (tests/app/PresetHost.h)
target_link_libraries(render_listen PRIVATE fw_dsp)
target_compile_options(render_listen PRIVATE -Wall -Wno-unused-parameter)
