if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD pico CACHE STRING "Board type")
cmake_minimum_required (VERSION 3.13)
include(pico_sdk_import.cmake)

set(CARD_NAME "nibbleko")
project(${CARD_NAME} C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# --- Optional baked-in PCM samples ---------------------------------------
# tools/mksamples.py writes samples.h from samples/*.raw (8-bit signed mono
# 48kHz, one file per voice slot — see tools/importwav.py). Neither the .raw
# files nor the generated header are committed: with no samples present the
# build still succeeds and every voice has nothing baked to fall back on
# (see samples_default.h). Ported from WorkshopBio/CMakeLists.txt.
find_package(Python3 COMPONENTS Interpreter)
set(SAMPLE_DIR ${CMAKE_CURRENT_LIST_DIR}/samples)
if (Python3_FOUND AND EXISTS ${SAMPLE_DIR})
  file(GLOB SAMPLE_FILES ${SAMPLE_DIR}/*.raw)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_LIST_DIR}/samples.h
    COMMAND ${Python3_EXECUTABLE} tools/mksamples.py samples samples.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    DEPENDS tools/mksamples.py ${SAMPLE_FILES}
    COMMENT "Generating samples.h from samples/*.raw")
  add_custom_target(pcm_samples DEPENDS ${CMAKE_CURRENT_LIST_DIR}/samples.h)
endif()

# webui.cpp is deliberately NOT built yet. It is a stub with no TinyUSB calls
# (see its header), so linking it would only add dead weight and invite the
# mistake of thinking USB works. It joins this list when the WebUI lands,
# together with tinyusb_device/tinyusb_board/pico_multicore below.
add_executable(${CARD_NAME}
  main.cpp
  levels.cpp
  drums.cpp
  looper.cpp
  fx.cpp
  fastmath.cpp)

if (TARGET pcm_samples)
  add_dependencies(${CARD_NAME} pcm_samples)
endif()
# drums.cpp reaches samples.h through samplestore.h -> samples_default.h.
# CMake cannot see that through a generated header, so name it explicitly:
# without this, re-importing a sample regenerates samples.h and nothing
# rebuilds, and you flash firmware that still contains the old audio.
if (TARGET pcm_samples)
  set_source_files_properties(drums.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/samples.h)
endif()

target_compile_options(${CARD_NAME} PRIVATE
  -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)

# Worth watching every build: a sudden jump means something large landed in
# the wrong section.
target_link_options(${CARD_NAME} PRIVATE -Wl,--print-memory-usage)

# REQUIRED for the Workshop Computer's crystal. Without it the card fails to
# boot on a cold power-up (it works from a warm reset, which is exactly what
# makes the bug so confusing). Do not remove.
target_compile_definitions(${CARD_NAME} PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(${CARD_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# TODO(webui): once webui.cpp's stub becomes a real TinyUSB implementation
# (see that file's header), this needs tinyusb_device, tinyusb_board and
# pico_multicore added — see WorkshopBio/CMakeLists.txt, whose "why there is
# USB and a second core here but not in NIBBLE" is the relevant precedent.
#
# hardware_flash stays regardless, and it is worth being precise about why:
# ComputerCard.h itself includes hardware/flash.h to read the factory CV
# output calibration. That dependency exists whether or not this card ever
# writes flash of its own — which it soon will, for the saved levels.
target_link_libraries(${CARD_NAME}
  pico_stdlib
  hardware_dma
  hardware_i2c
  hardware_pwm
  hardware_adc
  hardware_spi
  hardware_vreg
  hardware_flash)

# TODO(calibstore/webui): once the saved calibration and user-sample uploads
# are real, guard the firmware/user-region boundary the way WorkshopBio does —
# PICO_FLASH_SIZE_BYTES=2097152 plus a POST_BUILD checksize.cmake call with
# -DLIMIT matching samplestore.h's kUserRegionOff.

pico_add_extra_outputs(${CARD_NAME})

# The Computer has no USB serial console in normal use, and enabling stdio
# over USB pulls in the whole TinyUSB stack.
pico_enable_stdio_usb(${CARD_NAME} 0)
