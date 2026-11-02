cmake_minimum_required(VERSION 3.20)
project(cathode_bench CXX)

set(CMAKE_CXX_STANDARD 17)
# fork diagnostics (engagement counters such as fork::Config::slotSkips); the firmware leaves them out
add_compile_definitions(NEMESIS_FORK_DIAGNOSTICS=1)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Path to the upstream checkout whose dsp/ layer is benchmarked (read-only; nothing is copied).
set(UPSTREAM_OLD "${CMAKE_CURRENT_SOURCE_DIR}/../ref/old" CACHE PATH "OLD upstream checkout (pre-split), kept for the old benchmarks")
# Current upstream: a read-only shallow clone of origin/main (see tools/sync_dsp.py).
set(UPSTREAM_DSP "${CMAKE_CURRENT_SOURCE_DIR}/../ref/upstream/sdk/dsp" CACHE PATH "current upstream dsp/")
# The firmware uses its own fork of the DSP; the fork-aware tools build from it.
set(FORK_DSP "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/dsp" CACHE PATH "forked dsp/ tree")

file(GLOB DSP_SRCS CONFIGURE_DEPENDS "${UPSTREAM_OLD}/dsp/src/*.cpp")
list(FILTER DSP_SRCS EXCLUDE REGEX "Convolution\\.cpp$") # the only JUCE-dependent file
file(GLOB_RECURSE PLAITS_SRCS CONFIGURE_DEPENDS "${UPSTREAM_OLD}/dsp/thirdparty/plaits/*.cc")

add_executable(cathode_bench bench.cpp ${DSP_SRCS} ${PLAITS_SRCS})
target_include_directories(cathode_bench PRIVATE
    "${UPSTREAM_OLD}/dsp/include" "${UPSTREAM_OLD}/dsp/thirdparty/plaits")
set_source_files_properties(${PLAITS_SRCS} PROPERTIES COMPILE_OPTIONS -w)
target_compile_options(cathode_bench PRIVATE -O2)

add_executable(heap_probe heap_probe.cpp ${DSP_SRCS} ${PLAITS_SRCS})
target_include_directories(heap_probe PRIVATE "${UPSTREAM_OLD}/dsp/include" "${UPSTREAM_OLD}/dsp/thirdparty/plaits")
set_source_files_properties(${PLAITS_SRCS} PROPERTIES COMPILE_OPTIONS -w)

add_executable(heap_parts heap_parts.cpp ${DSP_SRCS} ${PLAITS_SRCS})
target_include_directories(heap_parts PRIVATE "${UPSTREAM_OLD}/dsp/include" "${UPSTREAM_OLD}/dsp/thirdparty/plaits")
set_source_files_properties(${PLAITS_SRCS} PROPERTIES COMPILE_OPTIONS -w)

file(GLOB FORK_SRCS CONFIGURE_DEPENDS "${FORK_DSP}/src/*.cpp")
file(GLOB_RECURSE FORK_PLAITS CONFIGURE_DEPENDS "${FORK_DSP}/thirdparty/plaits/*.cc")
add_executable(fork_check fork_check.cpp ${FORK_SRCS} ${FORK_PLAITS})
target_include_directories(fork_check PRIVATE "${FORK_DSP}/include" "${FORK_DSP}/thirdparty/plaits")
set_source_files_properties(${FORK_PLAITS} PROPERTIES COMPILE_OPTIONS -w)

# render_ref: same source against upstream, the fork in double, and the fork in float.
function(add_render_ref name src_dir float_mode is_fork fast_math)
    file(GLOB _s CONFIGURE_DEPENDS "${src_dir}/src/*.cpp")
    list(FILTER _s EXCLUDE REGEX "Convolution\\.cpp$")
    file(GLOB_RECURSE _p CONFIGURE_DEPENDS "${src_dir}/thirdparty/plaits/*.cc")
    # A static library, so the linker only pulls the objects the renderer references (the master-bus code needs
    # Convolution, which is JUCE-only and not part of the CATHODE voice).
    add_library(${name}_dsp STATIC ${_s} ${_p})
    add_executable(${name} render_ref.cpp)
    add_executable(${name}_scn render_scn.cpp)
    add_executable(${name}_cov render_cov.cpp)
    target_link_libraries(${name}_scn PRIVATE ${name}_dsp)
    target_link_libraries(${name}_cov PRIVATE ${name}_dsp)
    target_link_libraries(${name} PRIVATE ${name}_dsp)
    foreach(t ${name} ${name}_scn ${name}_cov ${name}_dsp)
        if(ARGN)
            target_compile_definitions(${t} PUBLIC ${ARGN})   # extra fork switches, e.g. NEMESIS_FORK_EXACT=1
        endif()
        target_include_directories(${t} PUBLIC "${src_dir}/include" "${src_dir}/thirdparty/plaits")
        if(float_mode)
            target_compile_definitions(${t} PUBLIC NEMESIS_DSP_FLOAT=1)
        endif()
        if(is_fork)
            target_compile_definitions(${t} PUBLIC DSP_IS_FORK=1)
        endif()
        if(fast_math)
            target_compile_definitions(${t} PUBLIC NEMESIS_DSP_FAST_MATH=1)
        endif()
    endforeach()
    set_source_files_properties(${_p} PROPERTIES COMPILE_OPTIONS -w)
    if(float_mode AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Same as the firmware: unsuffixed literals are float in the converted files, so Real math is not silently
        # promoted to double. clang IGNORES this flag (it prints "not supported"), so float builds under clang run with
        # double constants and are more accurate than the real thing -- only the GCC build is a faithful check.
        foreach(_f ${_s})
            file(READ ${_f} _txt)
            string(FIND "${_txt}" "nemesis/dsp/Real.h" _pos)
            if(NOT _pos EQUAL -1)
                set_source_files_properties(${_f} PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
            endif()
        endforeach()
        set_source_files_properties(render_ref.cpp render_scn.cpp render_cov.cpp PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
    endif()
endfunction()
add_render_ref(render_ref_upstream "${UPSTREAM_DSP}" OFF OFF OFF)
add_render_ref(render_ref_fork_double "${FORK_DSP}" OFF ON OFF)
add_render_ref(render_ref_fork_float "${FORK_DSP}" ON ON OFF)
add_render_ref(render_ref_fork_float_fast "${FORK_DSP}" ON ON ON)
# The DSP-review optimizations (docs/DSP_REVIEW.md): EXACT ones must be bit-identical to the same build without them,
# APPROX ones (1 ulp .. 1e-8) are checked spectrally. The firmware builds with both on (fw = float + fast math + exact + approx).
add_render_ref(render_ref_fork_double_exact "${FORK_DSP}" OFF ON OFF NEMESIS_FORK_EXACT=1)
add_render_ref(render_ref_fork_float_exact "${FORK_DSP}" ON ON ON NEMESIS_FORK_EXACT=1)
add_render_ref(render_ref_fork_float_fw "${FORK_DSP}" ON ON ON NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1)
add_render_ref(render_ref_fork_float_fw8 "${FORK_DSP}" ON ON ON NEMESIS_FORK_EXACT=1 NEMESIS_FORK_APPROX=1 NEMESIS_FORK_MAX_VOICES=8)
add_executable(cov_diff cov_diff.cpp)
target_compile_options(cov_diff PRIVATE -O2)

add_executable(fastmath_test fastmath_test.cpp)
target_include_directories(fastmath_test PRIVATE "${FORK_DSP}/include")
target_compile_definitions(fastmath_test PRIVATE NEMESIS_DSP_FLOAT=1 NEMESIS_DSP_FAST_MATH=1)
target_compile_options(fastmath_test PRIVATE -O2)

# pitch_probe against the fork in double (clang) and float (GCC build dir only)
file(GLOB _pp_src CONFIGURE_DEPENDS "${FORK_DSP}/src/*.cpp")

# heap footprint of the NEW fork's engine (lazy macro off / on)
foreach(_lazy 0 1)
    add_executable(heap_probe_lazy${_lazy} heap_probe.cpp)
    target_link_libraries(heap_probe_lazy${_lazy} PRIVATE render_ref_fork_double_dsp)
    target_compile_definitions(heap_probe_lazy${_lazy} PRIVATE LAZY=${_lazy})
endforeach()

foreach(_v fw fw8)
    add_executable(heap_probe_${_v} heap_probe.cpp)
    target_link_libraries(heap_probe_${_v} PRIVATE render_ref_fork_float_${_v}_dsp)
    target_compile_definitions(heap_probe_${_v} PRIVATE LAZY=1)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(heap_probe_${_v} PRIVATE -fsingle-precision-constant)
    endif()
endforeach()
add_executable(heap_probe_float heap_probe.cpp)
target_link_libraries(heap_probe_float PRIVATE render_ref_fork_float_dsp)
target_compile_definitions(heap_probe_float PRIVATE LAZY=1)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(heap_probe_float PRIVATE -fsingle-precision-constant)
endif()

# per-feature cost of one voice: double (clang, host) and float (GCC build dir, closer to the firmware's op mix)
add_executable(cost_bench_double cost_bench.cpp)
target_link_libraries(cost_bench_double PRIVATE render_ref_fork_double_dsp)
add_executable(cost_bench_float cost_bench.cpp)
target_link_libraries(cost_bench_float PRIVATE render_ref_fork_float_dsp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(cost_bench.cpp PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
endif()

# the firmware's DSP configuration (float, fast math, exact + approx optimizations, 8 voices): what the cost table is generated from
add_executable(cost_bench_ff cost_bench.cpp)
target_link_libraries(cost_bench_ff PRIVATE render_ref_fork_float_fast_dsp)
add_executable(cost_bench_fw cost_bench.cpp)
target_link_libraries(cost_bench_fw PRIVATE render_ref_fork_float_fw8_dsp)

add_executable(base_loop base_loop.cpp)
target_link_libraries(base_loop PRIVATE render_ref_fork_double_dsp)
target_compile_options(base_loop PRIVATE -g)

add_executable(slots_probe slots_probe.cpp)
target_link_libraries(slots_probe PRIVATE render_ref_fork_double_dsp)

add_executable(savings_check savings_check.cpp)
target_link_libraries(savings_check PRIVATE render_ref_fork_double_dsp)

add_executable(alias_check alias_check.cpp)
target_link_libraries(alias_check PRIVATE render_ref_fork_double_dsp)

add_executable(savings_check_float savings_check.cpp)
target_link_libraries(savings_check_float PRIVATE render_ref_fork_float_dsp)
add_executable(alias_check_float alias_check.cpp)
target_link_libraries(alias_check_float PRIVATE render_ref_fork_float_dsp)
add_executable(alias_check_ff alias_check.cpp)
target_link_libraries(alias_check_ff PRIVATE render_ref_fork_float_fast_dsp)
add_executable(alias_check_fw alias_check.cpp)
target_link_libraries(alias_check_fw PRIVATE render_ref_fork_float_fw_dsp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(savings_check.cpp alias_check.cpp PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
endif()

add_executable(model_check model_check.cpp)
target_link_libraries(model_check PRIVATE render_ref_fork_double_dsp)
target_include_directories(model_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src")

add_executable(model_check_fw model_check.cpp)
target_link_libraries(model_check_fw PRIVATE render_ref_fork_float_fw8_dsp)
target_include_directories(model_check_fw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src")
add_executable(degrade_check_fw degrade_check.cpp)
target_link_libraries(degrade_check_fw PRIVATE render_ref_fork_float_fw8_dsp)
target_include_directories(degrade_check_fw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(model_check_fw PRIVATE -fsingle-precision-constant)
    target_compile_options(degrade_check_fw PRIVATE -fsingle-precision-constant)
    target_compile_options(cost_bench_fw PRIVATE -fsingle-precision-constant)
    target_compile_options(cost_bench_ff PRIVATE -fsingle-precision-constant)
endif()
add_executable(degrade_check degrade_check.cpp)
target_link_libraries(degrade_check PRIVATE render_ref_fork_double_dsp)
target_include_directories(degrade_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src")
