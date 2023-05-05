cmake_minimum_required(VERSION 3.13)

SET(CMAKE_C_LINK_FLAGS "")
SET(CMAKE_CXX_LINK_FLAGS "")
SET(CMAKE_ASM_LINK_FLAGS "")

SET(CMAKE_C_FLAGS "-std=gnu11")
SET(CMAKE_CXX_FLAGS "-std=gnu++11 -fno-rtti -fno-exceptions")
SET(CMAKE_ASM_FLAGS "")

project(gsmsim)

message("CMAKE_SYSTEM_NAME ${CMAKE_SYSTEM_NAME}")
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  add_compile_options(-Wno-format -Wno-gnu-variable-sized-type-not-at-end -Wno-unknown-warning-option)
  SET(STRIP_FLAGS "-Wl,-dead_strip")
  set(MACOS_LIBS "-Wl,-framework,IOKit -Wl,-framework,CoreFoundation -Wl,-framework,Security")
  SET(PLATFORM_LINKER_FLAGS "${STRIP_FLAGS} ${MACOS_LIBS} -Wl,-map,<TARGET>.map -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  SET(PLATFORM_LINKER_FLAGS "-Wl,-s -lpthread -lutil -ludev -Wl,--gc-sections -Wl,--as-needed -s -Wl,-Map=<TARGET>.map")
endif()

SET(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <OBJECTS> -o <TARGET> ${PLATFORM_LINKER_FLAGS} <LINK_LIBRARIES>")
SET(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_LINK_EXECUTABLE}")

add_compile_options(
  -Os
  -ggdb
  -fstrict-aliasing
  -ffunction-sections
  -ffreestanding
  -fomit-frame-pointer
)

add_compile_options(
  -Wall
  -Wno-switch
  -Wno-multichar
  -Wno-unused-value
  -Wno-unused-result
  -Wno-unused-function
  -Wno-unused-variable
  -Wno-unused-but-set-variable
)

set(DEPS_SRC_DIR $ENV{DEPS_SRC_DIR})
set(DEPS_BUILD_PATH $ENV{DEPS_BUILD_PATH})

function(SETVAR NAME VALUE)
  SET("${NAME}" "${VALUE}" CACHE STRING "" FORCE)
endfunction()

function(register_lib dep inc lib)
  string(TOUPPER ${dep} DEP)
  if(NOT DEFINED ARGV3)
    SET(dir "${dep}")
  else()
    SET(dir "${ARGV3}/${dep}")
  endif()
  SETVAR("${DEP}_DIR" "${DEPS_SRC_DIR}/${dir}")
  SETVAR("${DEP}_INCLUDES" "${DEPS_SRC_DIR}/${dir}/${inc}")
  SETVAR("${DEP}_STATIC_LIB" "${DEPS_BUILD_PATH}/${dir}/${lib}")
endfunction()

register_lib("libusb" "libusb" "libusb/.libs/libusb-1.0.a")

SET(libs_dir "${CMAKE_CURRENT_SOURCE_DIR}/libs")

FILE(GLOB lib_dirs RELATIVE ${libs_dir} ${libs_dir}/*)
FOREACH(lib_dir ${lib_dirs})
  message("lib_dir ${lib_dir}")
  SET(dir ${libs_dir}/${lib_dir})
  IF(IS_DIRECTORY ${dir})
    file(GLOB_RECURSE ${lib_dir}_src CONFIGURE_DEPENDS "${dir}/*.c")
    add_library(${lib_dir} ${${lib_dir}_src})
  ENDIF()
ENDFOREACH()

target_link_libraries(usb PUBLIC ${LIBUSB_STATIC_LIB})
target_include_directories(usb PUBLIC ${LIBUSB_INCLUDES})

target_link_libraries(devices PUBLIC usb)

add_executable(app "main.c")
target_include_directories(app PUBLIC libs)
target_link_libraries(app PUBLIC at cmux util nmea devices)
