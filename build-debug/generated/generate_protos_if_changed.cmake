set(PROTOC_EXECUTABLE "/usr/bin/protoc")
set(CMAKE_COMMAND_PATH "/usr/bin/cmake")
set(CMAKE_SOURCE_DIR_PATH "/home/arthur/文档/GitHub/HackMatrixOptimized")
set(PROTO_CPP_OUTPUT_DIR "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated")
set(PROTO_CPP_SOURCE "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated/protos/api.pb.cc")
set(PROTO_CPP_HEADER "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated/protos/api.pb.h")
set(PROTO_TMP_OUTPUT_DIR "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated-tmp")
set(PROTO_PYTHON_OUTPUT_DIR "/home/arthur/文档/GitHub/HackMatrixOptimized/client_libs/python/hackMatrix")
set(PROTO_PYTHON_MODULE "/home/arthur/文档/GitHub/HackMatrixOptimized/client_libs/python/hackMatrix/protos/api_pb2.py")
set(PROTO_HASH_FILE "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated/proto-inputs.sha256")
set(PROTO_STAMP_FILE "/home/arthur/文档/GitHub/HackMatrixOptimized/build-debug/generated/protos.stamp")
set(PROTO_TOOLCHAIN_FINGERPRINT "protoc=36.1;libprotobuf=36.1.0")
set(PROTO_FILES
  "/home/arthur/文档/GitHub/HackMatrixOptimized/protos/api.proto"
)

set(PROTO_INPUT_HASH "toolchain:${PROTO_TOOLCHAIN_FINGERPRINT}\n")
foreach(PROTO_FILE IN LISTS PROTO_FILES)
  file(SHA256 "${PROTO_FILE}" PROTO_FILE_HASH)
  string(APPEND PROTO_INPUT_HASH "${PROTO_FILE}:${PROTO_FILE_HASH}\n")
endforeach()

set(SHOULD_GENERATE TRUE)
if(EXISTS "${PROTO_HASH_FILE}")
  file(READ "${PROTO_HASH_FILE}" PREVIOUS_PROTO_INPUT_HASH)
  if(PREVIOUS_PROTO_INPUT_HASH STREQUAL PROTO_INPUT_HASH
     AND EXISTS "${PROTO_CPP_SOURCE}"
     AND EXISTS "${PROTO_CPP_HEADER}"
     AND EXISTS "${PROTO_PYTHON_MODULE}")
    set(SHOULD_GENERATE FALSE)
  endif()
endif()

if(SHOULD_GENERATE)
  file(MAKE_DIRECTORY "${PROTO_CPP_OUTPUT_DIR}")
  file(MAKE_DIRECTORY "${PROTO_CPP_OUTPUT_DIR}/protos")
  file(MAKE_DIRECTORY "${PROTO_TMP_OUTPUT_DIR}")
  file(MAKE_DIRECTORY "${PROTO_TMP_OUTPUT_DIR}/protos")
  file(MAKE_DIRECTORY "${PROTO_PYTHON_OUTPUT_DIR}/protos")

  execute_process(
    COMMAND
      "${PROTOC_EXECUTABLE}"
      "--proto_path=${CMAKE_SOURCE_DIR_PATH}"
      "--cpp_out=${PROTO_TMP_OUTPUT_DIR}"
      ${PROTO_FILES}
    RESULT_VARIABLE PROTO_CPP_RESULT
  )
  if(NOT PROTO_CPP_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate protobuf C++ sources.")
  endif()

  execute_process(
    COMMAND
      "${PROTOC_EXECUTABLE}"
      "--proto_path=${CMAKE_SOURCE_DIR_PATH}"
      "--python_out=${PROTO_TMP_OUTPUT_DIR}"
      ${PROTO_FILES}
    RESULT_VARIABLE PROTO_PYTHON_RESULT
  )
  if(NOT PROTO_PYTHON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate protobuf Python sources.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api.pb.cc"
      "${PROTO_CPP_SOURCE}"
    RESULT_VARIABLE COPY_CPP_RESULT
  )
  if(NOT COPY_CPP_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf C++ source.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api.pb.h"
      "${PROTO_CPP_HEADER}"
    RESULT_VARIABLE COPY_HEADER_RESULT
  )
  if(NOT COPY_HEADER_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf C++ header.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api_pb2.py"
      "${PROTO_PYTHON_MODULE}"
    RESULT_VARIABLE COPY_PYTHON_RESULT
  )
  if(NOT COPY_PYTHON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf Python module.")
  endif()

  file(WRITE "${PROTO_HASH_FILE}" "${PROTO_INPUT_HASH}")
endif()

file(TOUCH "${PROTO_STAMP_FILE}")
