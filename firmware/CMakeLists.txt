cmake_minimum_required(VERSION 3.20)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake_arm.cmake)
project(dboy_cathode_firmware C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)   # gnu++17: newlib only defines M_PI in GNU mode (Plaits needs it)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# D-cache + MPU (src/memory/Mpu.cpp). Measured on the board: the default project plays at 877% of the audio budget without it.
option(DBOY_DCACHE "Enable the D-cache (the SDRAM and the SD driver are set up for it)" ON)
if(DBOY_DCACHE)
    set(DCACHE_VALUE 1)
else()
    set(DCACHE_VALUE 0)
endif()

# Frames the audio interrupt renders at a time (the I2S DMA half-buffer): 32 = 0.73 ms. A bigger block lets one expensive moment (a note start is
# ~300k cycles, nearly a whole 32-frame block) borrow time from the cheap ones around it, at the cost of latency (2 blocks). A power of two.
set(DBOY_AUDIO_BLOCK 32 CACHE STRING "Audio block (I2S half-buffer) in frames")

# A mailbox in RAM for tools/board_script.py (src/board/DebugHook.h): key events in, status and screen strips out, through the debug probe.
option(DBOY_DEBUG_HOOK "Build the debug mailbox for tools/board_script.py" ON)
if(DBOY_DEBUG_HOOK)
    set(DEBUG_HOOK_VALUE 1)
else()
    set(DEBUG_HOOK_VALUE 0)
endif()

# Optimisation level of the Release build. -O3 does not fit: it adds 235 KB to a flash region with 11 KB to spare and overflows the ITCM.
set(DBOY_OPT_LEVEL "-O2" CACHE STRING "Release optimisation flag")

# Single-precision DSP (dsp/include/nemesis/dsp/Real.h). The M7 FPU does float in hardware; double is several times
# slower and libm double routines are far worse. OFF builds the upstream double-precision DSP for comparison.
option(DBOY_FLOAT_DSP "Build the CATHODE DSP in single precision" ON)
# Cheap exp2/log2/tan/log1p/expm1 (dsp/include/nemesis/dsp/FastMath.h) in place of newlib's float libm. Needs FLOAT_DSP.
option(DBOY_FAST_MATH "Use fast approximations for per-sample transcendentals" ON)

# CPU savings in the DSP fork (dsp/include/nemesis/dsp/ForkConfig.h; measured in docs/CATHODE_COST.md).
#   IDLE_MATRIX and SKIP_SILENT_SLOTS are exact: output is bit-identical (bench/savings_check proves it).
#   FILTER_OVERSAMPLE 2 costs about 1 dB of alias floor on the Classic core and nothing on Acid, and saves 26-29% of a Classic voice.
#   PD_OVERSAMPLE 2 would save another ~14% of a voice but raises the alias floor by about 5 dB on bright high notes: opt-in.
option(DBOY_IDLE_MATRIX "Skip the mod matrix while it has no active route (exact)" ON)
option(DBOY_SKIP_SILENT_SLOTS "Do not compute a silent, uncoupled oscillator slot (exact while the level stays 0)" ON)
set(DBOY_FILTER_OVERSAMPLE 2 CACHE STRING "Oversampling of the non-linear filter cores (upstream 4)")
set(DBOY_PD_OVERSAMPLE 4 CACHE STRING "Oversampling of the phase-distortion oscillator (upstream 4)")
# docs/DSP_REVIEW.md: drive / output drive / Ladder at x2 measured no worse in alias floor (bench/alias_check_fw).
set(DBOY_DRIVE_OVERSAMPLE 2 CACHE STRING "Oversampling of the filter drive, output drive and Ladder core (upstream 4)")
# 0 = Classic follows FILTER_OVERSAMPLE. At x2 its top cutoff at full resonance is ~15.5 kHz (x4: ~31 kHz).
set(DBOY_CLASSIC_OVERSAMPLE 0 CACHE STRING "Oversampling of the Classic core, 0 = as FILTER_OVERSAMPLE")
# The DSP-review optimizations (docs/DSP_REVIEW.md): EXACT = bit-identical (bench/render_cov + cov_diff --exact),
# APPROX = ~1 ulp .. 1e-8 (checked spectrally and for aliasing). MAX_VOICES: the engine's prepared voice pool (upstream 32).
option(DBOY_FORK_EXACT "Bit-identical DSP optimizations" ON)
option(DBOY_FORK_APPROX "Nearly exact DSP optimizations (needs DBOY_FORK_EXACT)" ON)
# Screen saver: the display and its backlight go off after this many milliseconds without a key, knob or MIDI action (0 = never). Board scripts (tools/board_script.py)
# do not count as activity, so a script left running for hours lets the panel go dark. The first action wakes it and is not delivered to the UI.
set(DBOY_SCREENSAVER_MS 300000 CACHE STRING "Screen saver delay in ms (0 = off)")

# E21 (tools/fork_patches.py): leave out what the panel cannot reach. FILTERS: the five filter cores no panel model selects are not prepared (SDRAM, ~0.7-0.95 MB; their
# models play as the SVF low-pass); SOURCES: LFO2/3, S&H, step modulator, Env3, delta-decay envelopes tick only while a slot reads them and the three drifters are not run.
# Both are bit-identical for every state the panel can reach (golden set of 2,643 scenarios, docs/BRINGUP.md), not for the full engine; OFF = upstream.
option(DBOY_PANEL_FILTERS "Do not prepare the filter cores the panel cannot select" ON)
option(DBOY_PANEL_SOURCES "Do not tick the modulation sources the panel cannot route" ON)

# E19 (tools/fork_patches.py): control rate. DBOY_CTL_RATE = N evaluates a sounding voice's modulation matrix and named destinations (cutoff, pitch, amp, PD ...) every
# N-th sample and holds them between (the sources still tick every sample); 1 = today. INEXACT: it changes the sound (a staircase at fs/N on every modulated
# parameter), so it needs listening; board (Sub Pressure, one voice): N=8 saves ~5 points of the audio budget per voice. DBOY_IDLE_CTL_RATE is the same for a silent
# voice the engine's idle skip is ticking (a configured but unused voice costs 11.4 points; 5.4 at 64): its output is exactly 0, only the step-limiter state a
# note inherits can differ.
set(DBOY_CTL_RATE 4 CACHE STRING "Control rate N of sounding voices' modulation (1 = every sample)")
set(DBOY_IDLE_CTL_RATE 64 CACHE STRING "Control rate N of idle-skipped voices")
# E18 (tools/fork_patches.py): padding at the end of each voice object, bytes. The voices are 16,536 B = 2 x 8 KB + 152 apart, so their hot fields land on the
# same cache sets; measured on the board, 4 voices of one part: 232.6% of the audio budget unpadded, 146.6% at 4096 (linear would be 136%), 160-166% at 1536-2048.
# It changes only where the voices sit; the output is bit-identical. 0 = upstream layout. Costs pad x voices x parts of SDRAM (96 KB at 4096).
set(DBOY_VOICE_PAD 4096 CACHE STRING "Bytes of padding per voice object (cache set spreading)")
# E20: the engine's voice pool. 6 loses nothing for the shipped library (a part plays at most notes x unison <= 6 there; T5/T8), 8 was upstream's; each voice is ~140 KB + the
# E18 pad, so 6 gives back ~0.84 MB of SDRAM. Not a multiple of 4: the lane groups are rounded up (tools/fork_patches.py E20).
set(DBOY_MAX_VOICES 6 CACHE STRING "Physical voice pool of the engine (upstream 32)")
# Half-rate reverb and delay: their cores run at half the sample rate between a 31-tap halfband down and up (src/fx/FxUtil.h): flat to 8 kHz, -6 dB at 11 kHz, 0.68 ms
# extra latency on the wet signal (taken off the delay time). INEXACT; the return above ~9 kHz is softer and the tails are a little darker. ON (owner 2026-10-29).
option(DBOY_FX_HALF_RATE "Run the reverb and delay cores at half the sample rate" ON)

# The lean voice (docs/LEAN_VOICE.md; src/lean): DBOY_LEAN builds it into the Instrument, DBOY_LEAN_DEFAULT starts the parts on it. Both stay on: the
# board has no room for the CATHODE engine now a project has seven parts. The host tests and the web build still have it.
option(DBOY_LEAN "Build the lean voice into the Instrument" ON)
option(DBOY_LEAN_DEFAULT "Play the CATHODE parts through the lean voice (needs DBOY_LEAN)" ON)
if(NOT (DBOY_LEAN AND DBOY_LEAN_DEFAULT))
    message(FATAL_ERROR "The CATHODE engine no longer fits: seven of them need more SDRAM than the board has (docs/LEAN_VOICE.md, Seven parts)")
endif()
option(DBOY_STEREO_TWIN "Prepare each voice's stereo right chain (unison width, Stereo route)" ON)

set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
set(PLAT ${CMAKE_CURRENT_SOURCE_DIR}/platform)
set(HAL  ${PLAT}/Drivers/STM32H7xx_HAL_Driver)

file(GLOB HAL_SRCS CONFIGURE_DEPENDS ${HAL}/Src/*.c)
# Template files shipped by ST that are not meant to be compiled.
list(FILTER HAL_SRCS EXCLUDE REGEX "_template\\.c$")
file(GLOB PLAT_SRCS CONFIGURE_DEPENDS ${PLAT}/Src/*.c)
# The CATHODE DSP is our own fork (firmware/dsp/), a snapshot of upstream dsp/ at 1b6ec830 plus changes marked
# FIREFORK. upstream is never touched. See dsp/PROVENANCE.md.
set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
file(GLOB CATHODE_SRCS CONFIGURE_DEPENDS ${DSP_DIR}/src/*.cpp)
file(GLOB_RECURSE PLAITS_SRCS CONFIGURE_DEPENDS ${DSP_DIR}/thirdparty/plaits/*.cc)
file(GLOB APP_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*/*.cpp)
# FatFs (ChaN's R0.16 with patches 1 and 2, ff.c unmodified after them) and ST's SD glue (FATFS/App, FATFS/Target)
file(GLOB FATFS_SRCS CONFIGURE_DEPENDS ${PLAT}/FatFs/src/*.c ${PLAT}/FatFs/App/*.c ${PLAT}/FatFs/Target/*.c)

# DBOY_COLD_OS: -Os on the code that is never in the audio path (the UI, graphics, board drivers, MIDI, the codec setup, ST's HAL, FatFs). The app region is 99% full, and
# this takes ~88 KB (10%) back without touching the engine's -O2. The audio functions and the ITCM list are unaffected (measured by T5, 2026-10-28).
option(DBOY_COLD_OS "Optimise the cold directories for size" ON)
if(DBOY_COLD_OS)
    file(GLOB _cold CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/board/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/midi/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/codec/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/board.cpp)
    set_property(SOURCE ${_cold} ${HAL_SRCS} ${PLAT_SRCS} ${FATFS_SRCS} APPEND PROPERTY COMPILE_OPTIONS -Os)
endif()

# itcm_functions.ld (tools/gen_itcm.py) is INCLUDEd by the linker script: relink when it changes.
add_executable(dboy_cathode.elf ${APP_SRCS} ${CATHODE_SRCS} ${PLAITS_SRCS} ${PLAT_SRCS} ${HAL_SRCS} ${FATFS_SRCS} ${PLAT}/Startup/startup_stm32h723zgtx.s)

target_include_directories(dboy_cathode.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PLAT}/Inc
    ${HAL}/Inc
    ${PLAT}/Drivers/CMSIS/Include
    ${PLAT}/Drivers/CMSIS/Device/ST/STM32H7xx/Include
    ${DSP_DIR}/include
    ${DSP_DIR}/thirdparty/plaits
    ${PLAT}/FatFs/src
    ${PLAT}/FatFs/App
    ${PLAT}/FatFs/Target)

target_compile_definitions(dboy_cathode.elf PRIVATE
    USE_HAL_DRIVER STM32H723xx $<$<CONFIG:Debug>:DEBUG>
    # Plaits keeps its RNG state in a thread_local. Bare metal has no thread pointer (__aeabi_read_tp), and
    # the RNG is only touched from the single audio context after init, so a plain static is equivalent.
    thread_local=
    DBOY_IDLE_MATRIX_VALUE=$<BOOL:${DBOY_IDLE_MATRIX}>
    DBOY_SKIP_SILENT_SLOTS_VALUE=$<BOOL:${DBOY_SKIP_SILENT_SLOTS}>
    DBOY_FILTER_OVERSAMPLE_VALUE=${DBOY_FILTER_OVERSAMPLE}
    DBOY_PD_OVERSAMPLE_VALUE=${DBOY_PD_OVERSAMPLE}
    DBOY_DRIVE_OVERSAMPLE_VALUE=${DBOY_DRIVE_OVERSAMPLE}
    DBOY_CLASSIC_OVERSAMPLE_VALUE=${DBOY_CLASSIC_OVERSAMPLE}
    DBOY_STEREO_TWIN_VALUE=$<BOOL:${DBOY_STEREO_TWIN}>
    FX_HALF_RATE=$<BOOL:${DBOY_FX_HALF_RATE}>
    DBOY_LEAN=$<BOOL:${DBOY_LEAN}>
    DBOY_LEAN_DEFAULT=$<BOOL:${DBOY_LEAN_DEFAULT}>
    DBOY_COST_LEAN=$<AND:$<BOOL:${DBOY_LEAN}>,$<BOOL:${DBOY_LEAN_DEFAULT}>>
    DBOY_OLD_ENGINE=0
    NEMESIS_FORK_EXACT=$<BOOL:${DBOY_FORK_EXACT}>
    NEMESIS_FORK_APPROX=$<BOOL:${DBOY_FORK_APPROX}>
    NEMESIS_FORK_MAX_VOICES=${DBOY_MAX_VOICES}
    NEMESIS_FORK_VOICE_PAD=${DBOY_VOICE_PAD}
    NEMESIS_FORK_PANEL_FILTERS=$<BOOL:${DBOY_PANEL_FILTERS}>
    NEMESIS_FORK_PANEL_SOURCES=$<BOOL:${DBOY_PANEL_SOURCES}>
    NEMESIS_FORK_CTL_RATE=${DBOY_CTL_RATE}
    SCREENSAVER_MS=${DBOY_SCREENSAVER_MS}
    NEMESIS_FORK_IDLE_CTL_RATE=${DBOY_IDLE_CTL_RATE}
    NEMESIS_FORK_FAST_DATA_DTCM=1
    ENABLE_DCACHE=${DCACHE_VALUE}
    # the HAL's SD timeouts are 0xFFFFFFFF by default: an abort on a stuck card would never return. Data in bus clocks, software in ms
    SDMMC_DATATIMEOUT=6000000U
    SDMMC_SWDATATIMEOUT=1000U
    DBOY_DEBUG_HOOK=${DEBUG_HOOK_VALUE}
    I2S_BLOCK_SIZE=${DBOY_AUDIO_BLOCK})

if(DBOY_FLOAT_DSP)
    target_compile_definitions(dboy_cathode.elf PRIVATE NEMESIS_DSP_FLOAT=1)
    if(DBOY_FAST_MATH)
        target_compile_definitions(dboy_cathode.elf PRIVATE NEMESIS_DSP_FAST_MATH=1)
    endif()
    # Unsuffixed literals in the DSP are float, so `x * 0.5` is not silently promoted to a double multiply.
    # CathodeRenderer.cpp is included because the engine's inline processSample() is compiled there.
    # Only files already converted to Real (they include Real.h); the FX sources are still double.
    # main.cpp is included too: it pulls the engine headers in through CathodeRenderer.h, and their inline code needs
    # the same literal typing as the .cpp files or it fails to compile (float vs double std::clamp).
    # The same for every file of ours that includes the engine (src/audio: the renderer, the instrument, the patch map).
    file(GLOB _audio_srcs ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/fx/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/lean/*.cpp)
    set(FLOAT_CONST_SRCS ${_audio_srcs} ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
    foreach(_f ${CATHODE_SRCS})
        file(READ ${_f} _txt)
        string(FIND "${_txt}" "nemesis/dsp/Real.h" _pos)
        if(NOT _pos EQUAL -1)
            list(APPEND FLOAT_CONST_SRCS ${_f})
        endif()
    endforeach()
    set_source_files_properties(${FLOAT_CONST_SRCS} PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
endif()

target_compile_options(dboy_cathode.elf PRIVATE ${CPU_FLAGS}
    -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-threadsafe-statics
    # sqrt/etc. inline to one VFP instruction instead of a libm call that sets errno (nothing here reads errno)
    -fno-math-errno
    -Wall -Wno-unused-parameter
    $<$<CONFIG:Release>:${DBOY_OPT_LEVEL}> $<$<CONFIG:Debug>:-Og -g3>)
# ST's HAL and CubeMX output are not held to our warning bar.
set_source_files_properties(${HAL_SRCS} ${PLAT_SRCS} ${PLAITS_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

# itcm_functions.ld (tools/gen_itcm.py) is INCLUDEd by the linker script: relink when either changes.
set_target_properties(dboy_cathode.elf PROPERTIES LINK_DEPENDS "${PLAT}/STM32H723ZGTX_FLASH.ld;${PLAT}/itcm_functions.ld")
target_link_options(dboy_cathode.elf PRIVATE ${CPU_FLAGS}
    -T${PLAT}/STM32H723ZGTX_FLASH.ld -L${PLAT} -specs=nano.specs -specs=nosys.specs
    -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/dboy_cathode.map -Wl,--print-memory-usage)

add_custom_command(TARGET dboy_cathode.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary dboy_cathode.elf dboy_cathode.bin
    COMMENT "Raw application image (flash at 0x08020000)")
