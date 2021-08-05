cmake_minimum_required(VERSION 3.16)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/dev/cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(p2pchat C)

set(CMAKE_C_STANDARD 11)

find_package(LibEvent REQUIRED)
find_package(Readline REQUIRED)

add_custom_command(
  OUTPUT  rpc_generated.c include/generated/rpc.h
  COMMAND mkdir -p include/generated && python ${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/event_rpcgen.py
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc.rpc include/generated/rpc.h rpc_generated.c &&
  echo >> include/generated/rpc.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc.rpc
)

add_library(p2pgenerated rpc_generated.c)

if (MSVC)
  add_compile_options(/W3 /WX)
else()
  add_compile_options(-Wall -Wextra -Werror -pedantic)
  add_definitions(-D__STDC_WANT_LIB_EXT1__=1)
endif()

file(GLOB_RECURSE SOURCES src/*.c)
add_executable(p2pchat ${SOURCES})

# We do this because the generated header file is expected to be in the same
# directory as the generated c file, but we generate them to different
# directories because we are SMRT
set_source_files_properties(rpc_generated.c PROPERTIES COMPILE_FLAGS
  -I${CMAKE_CURRENT_BINARY_DIR}/include/generated)

target_include_directories(p2pchat PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${LIBEVENT_INCLUDE_DIR}
  ${Readline_INCLUDE_DIR}
  )
target_link_libraries(p2pchat ${LIBEVENT_LIB} ${Readline_LIBRARY} p2pgenerated)
