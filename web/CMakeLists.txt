cmake_minimum_required(VERSION 3.20)
project(dboy_web CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The Drumboy's instrument in the browser. Same sources as the host tests (tests/CMakeLists.txt): the CATHODE fork with the firmware's settings,
# the app core, the sampler, the effects. Build with Emscripten:  emcmake cmake -S web -B build-web && cmake --build build-web
# Nothing here changes the firmware's build.
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FW ${ROOT}/firmware)
set(FORK_DSP ${FW}/dsp)

file(GLOB FW_DSP_SRCS CONFIGURE_DEPENDS "${FORK_DSP}/src/*.cpp")
file(GLOB_RECURSE FW_DSP_PLAITS CONFIGURE_DEPENDS "${FORK_DSP}/thirdparty/plaits/*.cc")
add_library(fw_dsp STATIC ${FW_DSP_SRCS} ${FW_DSP_PLAITS})
# the settings the board's firmware runs the engine with (firmware/CMakeLists.txt: control rate 4, idle voices 64)
target_compile_definitions(fw_dsp PUBLIC NEMESIS_FORK_CTL_RATE=4 NEMESIS_FORK_IDLE_CTL_RATE=64)
target_include_directories(fw_dsp PUBLIC "${FORK_DSP}/include" "${FORK_DSP}/thirdparty/plaits")
target_compile_definitions(fw_dsp PUBLIC NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1 NEMESIS_FORK_MAX_VOICES=8 DSP_IS_FORK=1)
set_source_files_properties(${FW_DSP_PLAITS} PROPERTIES COMPILE_OPTIONS -w)

set(FW_APP ${FW}/src/app/Model.cpp ${FW}/src/app/FactoryProjects.cpp ${FW}/src/app/Params.cpp ${FW}/src/app/SampleLib.cpp ${FW}/src/app/CostBridge.cpp
    ${FW}/src/app/Ui.cpp ${FW}/src/app/UiTracker.cpp ${FW}/src/app/Sequencer.cpp ${FW}/src/app/MidiExport.cpp ${FW}/src/app/Slicer.cpp ${FW}/src/app/Looper.cpp ${FW}/src/app/ProjectFile.cpp ${FW}/src/app/MidiMap.cpp ${FW}/src/app/FxParams.cpp ${FW}/src/app/WaveLib.cpp)
set(FW_AUDIO ${FW}/src/lean/LeanPart.cpp ${FW}/src/audio/PatchMap.cpp ${FW}/src/audio/WaveStore.cpp ${FW}/src/audio/Sampler.cpp ${FW}/src/audio/Instrument.cpp ${FW}/src/audio/Recorder.cpp ${FW}/src/audio/SongExport.cpp
    ${FW}/src/fx/FxDelay.cpp ${FW}/src/fx/FxReverb.cpp ${FW}/src/fx/FxChorus.cpp ${FW}/src/fx/FxComp.cpp ${FW}/src/fx/FxAmp.cpp ${FW}/src/fx/FxSection.cpp)

# The instrument for the browser: one module for a Web Worker (web/www/engine-worker.js); the page, the worklet and the data are in web/www.
set(FW_GFX ${FW}/src/gfx/Canvas.cpp ${FW}/src/gfx/Screen.cpp ${FW}/src/gfx/BootImage.cpp ${FW}/src/gfx/fonts/Fonts.cpp)
add_executable(drumboy ${CMAKE_CURRENT_SOURCE_DIR}/src/web_api.cpp ${FW_APP} ${FW_AUDIO} ${FW_GFX})
target_include_directories(drumboy PRIVATE ${FW}/src ${ROOT}/tests/app)
target_link_libraries(drumboy PRIVATE fw_dsp)
target_compile_definitions(drumboy PRIVATE DBOY_LEAN=1 DBOY_COST_LEAN=1)
target_compile_options(drumboy PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(drumboy PRIVATE -sMODULARIZE=1 -sEXPORT_NAME=createDrumboy -sENVIRONMENT=worker,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=96MB -sSTACK_SIZE=2MB
    -sFILESYSTEM=0 -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,UTF8ToString -sEXPORTED_FUNCTIONS=_malloc,_free)
set_target_properties(drumboy PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/www)

# The same instrument with the reverb and delay cores at half the sample rate (src/fx/FxUtil.h, FX_HALF_RATE): the page switches between the two modules to hear what it costs in sound.
add_executable(drumboy_hr ${CMAKE_CURRENT_SOURCE_DIR}/src/web_api.cpp ${FW_APP} ${FW_AUDIO} ${FW_GFX})
target_include_directories(drumboy_hr PRIVATE ${FW}/src ${ROOT}/tests/app)
target_link_libraries(drumboy_hr PRIVATE fw_dsp)
target_compile_definitions(drumboy_hr PRIVATE DBOY_LEAN=1 DBOY_COST_LEAN=1 FX_HALF_RATE=1)
target_compile_options(drumboy_hr PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(drumboy_hr PRIVATE -sMODULARIZE=1 -sEXPORT_NAME=createDrumboy -sENVIRONMENT=worker,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=96MB -sSTACK_SIZE=2MB
    -sFILESYSTEM=0 -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,UTF8ToString -sEXPORTED_FUNCTIONS=_malloc,_free)
set_target_properties(drumboy_hr PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/www)

# The null test: the host's render_song, compiled to WebAssembly and run in Node against the repo's files (web/check_web.sh compares its WAV with the native one).
set(PRESETS ${ROOT}/content)
add_executable(render_song_wasm ${ROOT}/tests/app/render_song.cpp ${FW_APP} ${FW_AUDIO})
target_include_directories(render_song_wasm PRIVATE ${FW}/src)
target_link_libraries(render_song_wasm PRIVATE fw_dsp)
target_compile_definitions(render_song_wasm PRIVATE
    DBOY_PRESET_DIR="${PRESETS}/presets"
    DBOY_PRESET_MORE="${PRESETS}/presets2:${PRESETS}/synthwave:${PRESETS}/techno:${PRESETS}/synthpop:${PRESETS}/trance:${PRESETS}/ebm:${PRESETS}/royksopp:${PRESETS}/delerium:${PRESETS}/voice")
target_compile_options(render_song_wasm PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(render_song_wasm PRIVATE -sNODERAWFS=1 -sENVIRONMENT=node -sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
