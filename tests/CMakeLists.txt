cmake_minimum_required(VERSION 3.20)
project(dboy_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Hardware-independent firmware code, tested on the host. Run: ctest --test-dir build
enable_testing()
add_compile_definitions(DBOY_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../content/presets"
    DBOY_PRESET_MORE="${CMAKE_CURRENT_SOURCE_DIR}/../content/presets2:${CMAKE_CURRENT_SOURCE_DIR}/../content/synthwave:${CMAKE_CURRENT_SOURCE_DIR}/../content/techno:${CMAKE_CURRENT_SOURCE_DIR}/../content/synthpop:${CMAKE_CURRENT_SOURCE_DIR}/../content/trance:${CMAKE_CURRENT_SOURCE_DIR}/../content/ebm:${CMAKE_CURRENT_SOURCE_DIR}/../content/royksopp:${CMAKE_CURRENT_SOURCE_DIR}/../content/delerium:${CMAKE_CURRENT_SOURCE_DIR}/../content/voice")   # the library the tests load: content/presets and the folders the card is also given (tests/app/PresetHost.h)
add_executable(midi_test midi/test_midi.cpp)
target_include_directories(midi_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
find_package(Threads REQUIRED)
target_link_libraries(midi_test PRIVATE Threads::Threads)
target_compile_options(midi_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME midi COMMAND midi_test)

# the real SD driver and FatFs on a modelled card, DMA and cache
enable_language(C)
set(FF ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/platform/FatFs)
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
add_executable(storage_test storage/test_storage.cpp storage/test_card.cpp storage/SdModel.cpp ${FW}/storage/CardCheck.cpp ${FW}/board/Card.cpp ${FW}/board/CardSoak.cpp
    ${FW}/app/Params.cpp ${FW}/app/FxParams.cpp ${FW}/app/WaveLib.cpp ${FW}/app/SampleLib.cpp ${FW}/app/Model.cpp ${FW}/app/FactoryProjects.cpp ${FW}/app/CostBridge.cpp ${FW}/app/ProjectFile.cpp
    ${FW}/app/MidiMap.cpp ${FF}/App/fatfs.c ${FF}/Target/sd_diskio.c ${FF}/Target/bsp_driver_sd.c
    ${FF}/src/ff.c ${FF}/src/ff_gen_drv.c ${FF}/src/diskio.c ${FF}/src/ffunicode.c)
target_include_directories(storage_test PRIVATE storage storage/hal ${FW} ${FF}/App ${FF}/Target ${FF}/src)
target_compile_definitions(storage_test PRIVATE ENABLE_DCACHE=1 SD_DMA_REACHES=sd_model_reaches DBOY_COST_LEAN=1)
set_source_files_properties(${FF}/App/fatfs.c ${FF}/Target/sd_diskio.c ${FF}/Target/bsp_driver_sd.c ${FF}/src/ff.c ${FF}/src/ff_gen_drv.c ${FF}/src/diskio.c
    ${FF}/src/ffunicode.c PROPERTIES COMPILE_OPTIONS -w)
add_test(NAME storage COMMAND storage_test)

add_executable(cost_test cost/test_cost.cpp)
target_include_directories(cost_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(cost_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME cost COMMAND cost_test)
# the same tests on the lean voice's table (the firmware's own, DBOY_COST_LEAN)
add_executable(cost_test_lean cost/test_cost.cpp)
target_include_directories(cost_test_lean PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(cost_test_lean PRIVATE DBOY_COST_LEAN=1)
target_compile_options(cost_test_lean PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME cost_lean COMMAND cost_test_lean)

add_executable(dump_cost cost/dump_cost.cpp)
target_include_directories(dump_cost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)

# The panel's app core (firmware/src/app): UI state machine, sequencer, project file. A port of the mockup's test suite.
add_executable(app_test app/test_app.cpp ../firmware/src/gfx/Canvas.cpp ../firmware/src/gfx/Screen.cpp ../firmware/src/gfx/fonts/Fonts.cpp
    ../firmware/src/app/Model.cpp ../firmware/src/app/FactoryProjects.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/SampleLib.cpp ../firmware/src/app/CostBridge.cpp
    ../firmware/src/app/Ui.cpp ../firmware/src/app/UiTracker.cpp ../firmware/src/app/Sequencer.cpp ../firmware/src/app/MidiExport.cpp ../firmware/src/app/Slicer.cpp ../firmware/src/app/Looper.cpp ../firmware/src/app/ProjectFile.cpp ../firmware/src/app/MidiMap.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp)
target_include_directories(app_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(app_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME app COMMAND app_test)
# the app on the lean voice's cost table, as the firmware builds it (DBOY_COST_LEAN)
add_executable(app_test_lean $<TARGET_PROPERTY:app_test,SOURCES>)
target_include_directories(app_test_lean PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(app_test_lean PRIVATE DBOY_COST_LEAN=1)
target_compile_options(app_test_lean PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME app_lean COMMAND app_test_lean)

# The firmware's screen drawn on the host (PPM files), to compare with the mockup's reference shots.
add_executable(render_screens app/render_screens.cpp ../firmware/src/app/Slicer.cpp ../firmware/src/app/Looper.cpp
    ../firmware/src/app/Model.cpp ../firmware/src/app/FactoryProjects.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/SampleLib.cpp ../firmware/src/app/CostBridge.cpp
    ../firmware/src/app/Ui.cpp ../firmware/src/app/UiTracker.cpp ../firmware/src/app/ProjectFile.cpp ../firmware/src/app/MidiMap.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp
    ../firmware/src/gfx/Canvas.cpp ../firmware/src/gfx/Screen.cpp ../firmware/src/gfx/fonts/Fonts.cpp)
target_include_directories(render_screens PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(render_screens PRIVATE -Wall -Wextra -Wno-unused-parameter)
# the same on the lean voice's cost table, as the firmware builds it: the numbers on the screens of the website (web/tools/make_screens.py) are the board's
add_executable(render_screens_lean $<TARGET_PROPERTY:render_screens,SOURCES>)
target_include_directories(render_screens_lean PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(render_screens_lean PRIVATE DBOY_COST_LEAN=1)
target_compile_options(render_screens_lean PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The boot image's decoder against the pixels the generator quantised, and the picture as a PNG to look at (render_boot out.png).
add_executable(boot_image_test app/test_boot_image.cpp ../firmware/src/gfx/BootImage.cpp)
target_include_directories(boot_image_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(boot_image_test PRIVATE DBOY_BOOT_REF="${CMAKE_CURRENT_SOURCE_DIR}/app/boot_image_ref.pgm")
target_compile_options(boot_image_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME boot_image COMMAND boot_image_test)
# the same under the address and undefined-behaviour sanitizers: damaged data must stay inside the decoder's arrays
add_executable(boot_image_test_asan app/test_boot_image.cpp ../firmware/src/gfx/BootImage.cpp)
target_include_directories(boot_image_test_asan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(boot_image_test_asan PRIVATE DBOY_BOOT_REF="${CMAKE_CURRENT_SOURCE_DIR}/app/boot_image_ref.pgm")
target_compile_options(boot_image_test_asan PRIVATE -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
target_link_options(boot_image_test_asan PRIVATE -fsanitize=address,undefined)
add_test(NAME boot_image_asan COMMAND boot_image_test_asan)
add_executable(render_boot app/render_boot.cpp ../firmware/src/gfx/BootImage.cpp)
target_include_directories(render_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(render_boot PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The whole instrument on the host: the CATHODE fork with the firmware's optimizations and 8-voice pool, in double (clang cannot build the
# float variant; bench/ checks float with GCC), the sampler, the sequencer. render_song plays a project into a WAV file.
set(FORK_DSP ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/dsp)
file(GLOB FW_DSP_SRCS CONFIGURE_DEPENDS "${FORK_DSP}/src/*.cpp")
file(GLOB_RECURSE FW_DSP_PLAITS CONFIGURE_DEPENDS "${FORK_DSP}/thirdparty/plaits/*.cc")
add_library(fw_dsp STATIC ${FW_DSP_SRCS} ${FW_DSP_PLAITS})
target_include_directories(fw_dsp PUBLIC "${FORK_DSP}/include" "${FORK_DSP}/thirdparty/plaits")
target_compile_definitions(fw_dsp PUBLIC NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1 NEMESIS_FORK_MAX_VOICES=8 DSP_IS_FORK=1)
set_source_files_properties(${FW_DSP_PLAITS} PROPERTIES COMPILE_OPTIONS -w)

add_executable(render_song app/render_song.cpp
    ../firmware/src/app/Model.cpp ../firmware/src/app/FactoryProjects.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/SampleLib.cpp ../firmware/src/app/CostBridge.cpp
    ../firmware/src/app/Ui.cpp ../firmware/src/app/UiTracker.cpp ../firmware/src/app/Sequencer.cpp ../firmware/src/app/MidiExport.cpp ../firmware/src/app/Slicer.cpp ../firmware/src/app/Looper.cpp ../firmware/src/app/ProjectFile.cpp ../firmware/src/app/MidiMap.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp
    ../firmware/src/audio/PatchMap.cpp ../firmware/src/audio/WaveStore.cpp ../firmware/src/audio/Sampler.cpp ../firmware/src/audio/Instrument.cpp
    ../firmware/src/fx/FxDelay.cpp ../firmware/src/fx/FxReverb.cpp ../firmware/src/fx/FxChorus.cpp ../firmware/src/fx/FxComp.cpp ../firmware/src/fx/FxAmp.cpp ../firmware/src/fx/FxSection.cpp)
target_include_directories(render_song PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_link_libraries(render_song PRIVATE fw_dsp)
target_compile_options(render_song PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The lean voice (firmware/src/lean, docs/LEAN_VOICE.md) on the host: a library preset through lean::Part, timed per voice-sample, written as a WAV.
add_executable(lean_bench lean/lean_bench.cpp ../firmware/src/lean/LeanPart.cpp ../firmware/src/audio/PatchMap.cpp
    ../firmware/src/app/Params.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp ../firmware/src/app/SampleLib.cpp ../firmware/src/app/CostBridge.cpp ../firmware/src/app/Model.cpp ../firmware/src/app/FactoryProjects.cpp)
target_include_directories(lean_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(lean_bench PRIVATE fw_dsp)
target_compile_options(lean_bench PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(fastmath_check lean/fastmath_check.cpp)
target_include_directories(fastmath_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_link_libraries(fastmath_check PRIVATE fw_dsp)
target_compile_options(fastmath_check PRIVATE -Wall -Wextra)
add_test(NAME lean_fastmath COMMAND fastmath_check)

add_executable(wt_pyramid ../tools/wt_pyramid.cpp)
target_link_libraries(wt_pyramid PRIVATE fw_dsp)
target_compile_options(wt_pyramid PRIVATE -Wall -Wextra)

add_executable(audio_test app/test_audio.cpp ../firmware/src/audio/Recorder.cpp ../firmware/src/audio/SongExport.cpp
    ../firmware/src/app/Model.cpp ../firmware/src/app/FactoryProjects.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/SampleLib.cpp ../firmware/src/app/CostBridge.cpp
    ../firmware/src/app/Ui.cpp ../firmware/src/app/UiTracker.cpp ../firmware/src/app/Sequencer.cpp ../firmware/src/app/MidiExport.cpp ../firmware/src/app/Slicer.cpp ../firmware/src/app/Looper.cpp ../firmware/src/app/ProjectFile.cpp ../firmware/src/app/MidiMap.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp
    ../firmware/src/audio/PatchMap.cpp ../firmware/src/audio/WaveStore.cpp ../firmware/src/audio/Sampler.cpp ../firmware/src/audio/Instrument.cpp
    ../firmware/src/fx/FxDelay.cpp ../firmware/src/fx/FxReverb.cpp ../firmware/src/fx/FxChorus.cpp ../firmware/src/fx/FxComp.cpp ../firmware/src/fx/FxAmp.cpp ../firmware/src/fx/FxSection.cpp)
target_include_directories(audio_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_link_libraries(audio_test PRIVATE fw_dsp)
target_compile_options(audio_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME audio COMMAND audio_test)
add_executable(audio_test_lean $<TARGET_PROPERTY:audio_test,SOURCES> ../firmware/src/lean/LeanPart.cpp)
target_include_directories(audio_test_lean PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_definitions(audio_test_lean PRIVATE DBOY_COST_LEAN=1 DBOY_LEAN=1 DBOY_LEAN_DEFAULT=1 DBOY_OLD_ENGINE=0)
target_link_libraries(audio_test_lean PRIVATE fw_dsp)
target_compile_options(audio_test_lean PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME audio_lean COMMAND audio_test_lean)

# The preset library on the card: the file format, the index, the lazy loading (firmware/src/app/PresetFile.h, Params.cpp).
# the website's real client talks to this: web/test/link.mjs
add_executable(link_bridge midi/link_bridge.cpp ../firmware/src/midi/PresetLink.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp ../firmware/src/app/SampleLib.cpp)
target_include_directories(link_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)

add_executable(preset_test app/test_presets.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp ../firmware/src/app/SampleLib.cpp)
target_include_directories(preset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(preset_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME preset COMMAND preset_test)
# The website's preset editor: its data, made from the firmware's tables (web/www/preset-schema.js), and the firmware's word on what the editor produces (web/test/preset-edit.mjs).
add_executable(preset_schema ../tools/gen_preset_schema.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp ../firmware/src/app/SampleLib.cpp)
target_include_directories(preset_schema PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(preset_schema PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_executable(preset_accept app/preset_accept.cpp ../firmware/src/app/Params.cpp ../firmware/src/app/FxParams.cpp ../firmware/src/app/WaveLib.cpp ../firmware/src/app/SampleLib.cpp)
target_include_directories(preset_accept PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
target_compile_options(preset_accept PRIVATE -Wall -Wextra -Wno-unused-parameter)
