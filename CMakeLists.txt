check_include_file_cxx(stdint.h HAVE_STDINT_H)
if(HAVE_STDINT_H)
    add_definitions(-DHAVE_STDINT_H)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("-Wno-gcc-compat")
endif()

option(NS3AI_ENABLE_PYTHON_BINDINGS "Build ns3-ai Python binding modules" ON)
set(NS3AI_GYM_MSG_BUFFER_SIZE "65536" CACHE STRING "Size in bytes of the fixed Gym interface shared-memory message buffer")
add_compile_definitions(NS3_AI_GYM_MSG_BUFFER_SIZE=${NS3AI_GYM_MSG_BUFFER_SIZE})

find_package(Boost REQUIRED COMPONENTS program_options)
include_directories(${Boost_INCLUDE_DIRS})
if(NS3AI_ENABLE_PYTHON_BINDINGS)
    find_package(Python REQUIRED COMPONENTS Interpreter Development)
    if (Python_FOUND)
        message(STATUS "Python include directories: ${Python_INCLUDE_DIRS}")
        message(STATUS "Python library directories: ${Python_LIBRARY_DIRS}")
    endif()
    find_package(pybind11 CONFIG REQUIRED)
    include_directories(${pybind11_INCLUDE_DIRS})
else()
    message(STATUS "ns3-ai Python binding modules disabled")
endif()

# we need CONFIG for debug mode in macOS
find_package(Protobuf CONFIG)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
include_directories(${Protobuf_INCLUDE_DIRS})

# Optional C++-based ML frameworks. These must be opted in with explicit CMake paths rather
# than by placing third-party binaries inside the ns3-ai source tree.
set(NS3AI_LIBTENSORFLOW_ROOT "" CACHE PATH "Optional TensorFlow C library root for ns3-ai pure C++ examples")
set(NS3AI_LIBTENSORFLOW_INCLUDE_DIR "" CACHE PATH "Optional TensorFlow C include directory")
set(NS3AI_LIBTENSORFLOW_LIBRARY_DIR "" CACHE PATH "Optional TensorFlow C library directory")
set(NS3AI_LIBTORCH_ROOT "" CACHE PATH "Optional libtorch root for ns3-ai pure C++ examples")
set(NS3AI_LIBTORCH_INCLUDE_DIRS "" CACHE STRING "Optional semicolon-separated libtorch include directories")
set(NS3AI_LIBTORCH_LIBRARY_DIR "" CACHE PATH "Optional libtorch library directory")

set(NS3AI_LIBTENSORFLOW_EXAMPLES OFF)
if(NS3AI_LIBTENSORFLOW_ROOT)
    set(NS3AI_LIBTENSORFLOW_INCLUDE_DIR "${NS3AI_LIBTENSORFLOW_ROOT}/include" CACHE PATH "Optional TensorFlow C include directory" FORCE)
    set(NS3AI_LIBTENSORFLOW_LIBRARY_DIR "${NS3AI_LIBTENSORFLOW_ROOT}/lib" CACHE PATH "Optional TensorFlow C library directory" FORCE)
endif()
if(NS3AI_LIBTENSORFLOW_INCLUDE_DIR OR NS3AI_LIBTENSORFLOW_LIBRARY_DIR)
    if(NOT IS_DIRECTORY "${NS3AI_LIBTENSORFLOW_INCLUDE_DIR}")
        message(FATAL_ERROR "NS3AI_LIBTENSORFLOW_INCLUDE_DIR must point to a valid TensorFlow C include directory")
    endif()
    if(NOT IS_DIRECTORY "${NS3AI_LIBTENSORFLOW_LIBRARY_DIR}")
        message(FATAL_ERROR "NS3AI_LIBTENSORFLOW_LIBRARY_DIR must point to a valid TensorFlow C library directory")
    endif()
    file(GLOB TensorFlow_LIBRARIES "${NS3AI_LIBTENSORFLOW_LIBRARY_DIR}/*.so" "${NS3AI_LIBTENSORFLOW_LIBRARY_DIR}/*.dylib")
    if(NOT TensorFlow_LIBRARIES)
        message(FATAL_ERROR "No TensorFlow shared libraries found in NS3AI_LIBTENSORFLOW_LIBRARY_DIR")
    endif()
    set(Libtensorflow_INCLUDE_DIR "${NS3AI_LIBTENSORFLOW_INCLUDE_DIR}")
    set(NS3AI_LIBTENSORFLOW_EXAMPLES ON)
    message(STATUS "TensorFlow C library configured explicitly; examples using libtensorflow are enabled")
else()
    message(STATUS "TensorFlow C library not configured; examples using libtensorflow are disabled")
endif()

set(NS3AI_LIBTORCH_EXAMPLES OFF)
if(NS3AI_LIBTORCH_ROOT)
    set(NS3AI_LIBTORCH_INCLUDE_DIRS
            "${NS3AI_LIBTORCH_ROOT}/include;${NS3AI_LIBTORCH_ROOT}/include/torch/csrc/api/include"
            CACHE STRING "Optional semicolon-separated libtorch include directories" FORCE)
    set(NS3AI_LIBTORCH_LIBRARY_DIR "${NS3AI_LIBTORCH_ROOT}/lib" CACHE PATH "Optional libtorch library directory" FORCE)
endif()
if(NS3AI_LIBTORCH_INCLUDE_DIRS OR NS3AI_LIBTORCH_LIBRARY_DIR)
    if(NOT NS3AI_LIBTORCH_INCLUDE_DIRS)
        message(FATAL_ERROR "NS3AI_LIBTORCH_INCLUDE_DIRS must be set when configuring libtorch")
    endif()
    foreach(dir ${NS3AI_LIBTORCH_INCLUDE_DIRS})
        if(NOT IS_DIRECTORY "${dir}")
            message(FATAL_ERROR "Libtorch include directory does not exist: ${dir}")
        endif()
    endforeach()
    if(NOT IS_DIRECTORY "${NS3AI_LIBTORCH_LIBRARY_DIR}")
        message(FATAL_ERROR "NS3AI_LIBTORCH_LIBRARY_DIR must point to a valid libtorch library directory")
    endif()
    file(GLOB Torch_LIBRARIES "${NS3AI_LIBTORCH_LIBRARY_DIR}/*.so" "${NS3AI_LIBTORCH_LIBRARY_DIR}/*.dylib")
    if(NOT Torch_LIBRARIES)
        message(FATAL_ERROR "No libtorch shared libraries found in NS3AI_LIBTORCH_LIBRARY_DIR")
    endif()
    set(Libtorch_INCLUDE_DIRS ${NS3AI_LIBTORCH_INCLUDE_DIRS})
    set(NS3AI_LIBTORCH_EXAMPLES ON)
    message(STATUS "PyTorch C++ library configured explicitly; examples using libtorch are enabled")
else()
    message(STATUS "PyTorch C++ library not configured; examples using libtorch are disabled")
endif()

set(NS3AI_GENERATED_CPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/model/gym-interface/cpp")
set(NS3AI_PYTHON_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}/python")
set(NS3AI_PYTHON_PACKAGE_BUILD_DIR "${NS3AI_PYTHON_BUILD_ROOT}/ns3ai_gym_env")
include_directories(${NS3AI_GENERATED_CPP_DIR})

set(msg_interface_srcs )
# Keep pybind-only helper headers out of build_lib HEADER_FILES: ns-3 generates an aggregate
# module header from this list, and including pybind11 there forces every C++ executable to link
# against libpython even when it does not use Python bindings.
set(msg_interface_hdrs model/msg-interface/ns3-ai-errors.h model/msg-interface/ns3-ai-msg-interface.h model/msg-interface/ns3-ai-semaphore.h)
set(gym_interface_srcs
        model/gym-interface/cpp/ns3-ai-gym-interface.cc
        model/gym-interface/cpp/ns3-ai-gym-env.cc
        model/gym-interface/cpp/ns3-ai-vector-gym-env.cc
        model/gym-interface/cpp/container.cc
        model/gym-interface/cpp/spaces.cc
        ${NS3AI_GENERATED_CPP_DIR}/messages.pb.cc
)
# Keep generated protobuf headers out of build_lib HEADER_FILES: ns-3 validates this list during
# configure, before custom commands have created messages.pb.h in the binary tree.
set(gym_interface_hdrs
        model/gym-interface/cpp/ns3-ai-gym-interface.h
        model/gym-interface/cpp/ns3-ai-gym-env.h
        model/gym-interface/cpp/ns3-ai-vector-gym-env.h
        model/gym-interface/cpp/container.h
        model/gym-interface/cpp/spaces.h
)

# protobuf_generate function is missing in some installations by package manager
check_function_exists(protobuf_generate protobuf_generate_exists)
if(${protobuf_generate_exists})
    message(STATUS "protobuf_generate function found")
else()
    message(STATUS "protobuf_generate function not found -> use a local copy from ${CMAKE_CURRENT_SOURCE_DIR}/protobuf-generate.cmake")
    include(${CMAKE_CURRENT_SOURCE_DIR}/protobuf-generate.cmake)
endif()

# Compile proto files without writing generated outputs back into the source tree.
add_library(proto-objects OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/model/gym-interface/messages.proto")
protobuf_generate(
        TARGET proto-objects
        IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/model/gym-interface"
        LANGUAGE cpp
        PROTOC_OUT_DIR "${NS3AI_GENERATED_CPP_DIR}"
)
if(NS3AI_ENABLE_PYTHON_BINDINGS)
    file(MAKE_DIRECTORY "${NS3AI_PYTHON_PACKAGE_BUILD_DIR}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/model/gym-interface/py/ns3ai_gym_env/"
         DESTINATION "${NS3AI_PYTHON_PACKAGE_BUILD_DIR}")
    set(NS3AI_PYTHON_PACKAGE_ROOT "${NS3AI_PYTHON_BUILD_ROOT}")
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/model/gym-interface/py/setup-build.py.in"
            "${NS3AI_PYTHON_BUILD_ROOT}/setup.py"
            @ONLY
    )
    protobuf_generate(
            TARGET proto-objects
            IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/model/gym-interface"
            LANGUAGE python
            PROTOC_OUT_DIR "${NS3AI_PYTHON_PACKAGE_BUILD_DIR}"
    )
    message(STATUS "ns3-ai Gym Python package build directory: ${NS3AI_PYTHON_BUILD_ROOT}")
endif()

# L2 集成测试使用 fork() / waitpid() / kill() / POSIX shared-memory，仅限 Unix
if(UNIX)
    set(NS3AI_L2_TEST_SOURCE test/ns3-ai-l2-integration-test-suite.cc)
endif()

build_lib(
        LIBNAME ai
        SOURCE_FILES ${msg_interface_srcs} ${gym_interface_srcs}
        HEADER_FILES ${msg_interface_hdrs} ${gym_interface_hdrs}
        LIBRARIES_TO_LINK ${libcore} protobuf::libprotobuf
        TEST_SOURCES test/ns3-ai-error-test-suite.cc test/msg-interface-session-lifecycle-test-suite.cc test/ns3-ai-semaphore-test-suite.cc test/ns3-ai-schema-validation-test-suite.cc test/ns3-ai-gym-lifecycle-test-suite.cc ${NS3AI_L2_TEST_SOURCE}
)
add_dependencies(${libai} proto-objects)

# Build Gym msg binding module
if(NS3AI_ENABLE_PYTHON_BINDINGS)
    add_subdirectory(model/gym-interface/py)
endif()
