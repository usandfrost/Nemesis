# Cortex-M7 (STM32H723) cross toolchain. ARM_GNU_ROOT may point at any Arm GNU Toolchain "Payload" dir.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ENV{ARM_GNU_ROOT})
    set(ENV{ARM_GNU_ROOT} "$ENV{HOME}/toolchains/arm-gnu-pkg/Payload")
endif()
set(_t "$ENV{ARM_GNU_ROOT}/bin/arm-none-eabi-")

set(CMAKE_C_COMPILER   ${_t}gcc)
set(CMAKE_CXX_COMPILER ${_t}g++)
set(CMAKE_ASM_COMPILER ${_t}gcc)
set(CMAKE_OBJCOPY      ${_t}objcopy CACHE INTERNAL "")
set(CMAKE_SIZE         ${_t}size    CACHE INTERNAL "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
