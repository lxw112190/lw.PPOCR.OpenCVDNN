cmake_minimum_required(VERSION 3.24)

project(lw.PPOCR.OpenCVDNN VERSION 0.4.0 LANGUAGES C CXX)

option(LW_PPOCR_BUILD_HTTP_SERVICE "Build the cpp-httplib HTTP service" ON)
option(LW_PPOCR_BUILD_EXAMPLES "Build native examples" ON)
option(LW_PPOCR_BUILD_TESTS "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set(CMAKE_BUILD_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs dnn)
if(OpenCV_VERSION VERSION_LESS 4.5)
    message(FATAL_ERROR "OpenCV 4.5 or newer is required")
endif()
find_package(Threads REQUIRED)

add_library(lw_ppocr SHARED
    src/api/api.cpp
    src/engine/ocr_engine.cpp
    src/core/model_manifest.cpp
    src/core/image_ops.cpp
    src/core/db_postprocessor.cpp
    third_party/clipper/clipper.cpp)

target_compile_definitions(lw_ppocr PRIVATE LW_PPOCR_BUILDING_LIBRARY)
target_include_directories(lw_ppocr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/core/include
        src/engine
        third_party/clipper
        third_party/nlohmann-json)
target_link_libraries(lw_ppocr PRIVATE ${OpenCV_LIBS} Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    # GCC 8 implements C++17 filesystem in a separate compatibility library.
    # This is required by UOS 20 / Debian 10-era toolchains.
    target_link_libraries(lw_ppocr PRIVATE stdc++fs)
endif()
set_target_properties(lw_ppocr PROPERTIES
    OUTPUT_NAME "lw.PPOCR.OpenCVDNN"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

if(WIN32)
    add_custom_command(TARGET lw_ppocr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:lw_ppocr>
            $<TARGET_FILE_DIR:lw_ppocr>
        COMMAND_EXPAND_LISTS)
endif()

if(MSVC)
    target_compile_options(lw_ppocr PRIVATE /W4 /utf-8 /EHsc)
else()
    target_compile_options(lw_ppocr PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LW_PPOCR_BUILD_HTTP_SERVICE)
    add_executable(lw-ppocr-http-service
        apps/http_service.cpp
        src/http/base64.cpp
        src/http/logging.cpp)
    target_include_directories(lw-ppocr-http-service PRIVATE
        include
        src/http
        third_party/cpp-httplib
        third_party/nlohmann-json
        third_party/spdlog/include)
    target_link_libraries(lw-ppocr-http-service PRIVATE lw_ppocr Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(lw-ppocr-http-service PRIVATE stdc++fs)
    endif()
    if(WIN32)
        target_link_libraries(lw-ppocr-http-service PRIVATE ws2_32)
        target_compile_options(lw-ppocr-http-service PRIVATE /utf-8 /EHsc)
    endif()
endif()

if(LW_PPOCR_BUILD_EXAMPLES)
    add_executable(lw-ppocr-c-example examples/c/main.c)
    target_include_directories(lw-ppocr-c-example PRIVATE include)
    target_link_libraries(lw-ppocr-c-example PRIVATE lw_ppocr)
endif()

include(CTest)
if(BUILD_TESTING AND LW_PPOCR_BUILD_TESTS)
    add_executable(lw-ppocr-api-test tests/api_test.cpp)
    target_include_directories(lw-ppocr-api-test PRIVATE include)
    target_link_libraries(lw-ppocr-api-test PRIVATE lw_ppocr)
    add_test(NAME api_version COMMAND lw-ppocr-api-test)
    add_executable(lw-ppocr-model-manifest-test
        tests/model_manifest_test.cpp
        src/core/model_manifest.cpp)
    target_include_directories(lw-ppocr-model-manifest-test PRIVATE
        src/core/include
        third_party/nlohmann-json)
    target_link_libraries(lw-ppocr-model-manifest-test PRIVATE ${OpenCV_LIBS})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(lw-ppocr-model-manifest-test PRIVATE stdc++fs)
    endif()
    add_test(NAME model_manifest_schema
        COMMAND lw-ppocr-model-manifest-test
            ${CMAKE_CURRENT_SOURCE_DIR}/models/ppocrv6-tiny/model.json)
endif()

include(GNUInstallDirs)
install(TARGETS lw_ppocr
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION lib)
if(LW_PPOCR_BUILD_HTTP_SERVICE)
    install(TARGETS lw-ppocr-http-service RUNTIME DESTINATION .)
endif()
if(LW_PPOCR_BUILD_EXAMPLES)
    install(TARGETS lw-ppocr-c-example RUNTIME DESTINATION .)
endif()
install(FILES include/lw/ppocr.h DESTINATION include/lw)
install(FILES models/model-manifest.schema.json DESTINATION models)
install(DIRECTORY models/ppocrv6-tiny DESTINATION models)
install(DIRECTORY www/ DESTINATION www)
install(FILES http-service.json DESTINATION .)
install(FILES README.md README.en.md LICENSE THIRD-PARTY-NOTICES.md DESTINATION .)
install(FILES docs/assets/sponsor.jpg DESTINATION docs/assets)
install(FILES docs/HTTP-API.md DESTINATION docs)
install(FILES docs/DOCKER.md DESTINATION docs)
install(FILES docs/LINUX-ARM64-UOS20.md DESTINATION docs)
install(FILES docs/LOGGING.md DESTINATION docs)
install(FILES docs/C-ABI.md DESTINATION docs)
install(FILES docs/MODEL-MANIFEST.md DESTINATION docs)
install(FILES tests/abi/exports-v1.txt DESTINATION docs)
install(DIRECTORY examples/python DESTINATION examples)
install(DIRECTORY examples/csharp DESTINATION examples
    PATTERN "bin" EXCLUDE
    PATTERN "obj" EXCLUDE)
install(DIRECTORY licenses/ DESTINATION licenses)
if(WIN32)
    install(FILES run-http-service.bat DESTINATION .)
    install(FILES
        deploy/windows/install-service.bat
        deploy/windows/uninstall-service.bat
        deploy/windows/start-service.bat
        deploy/windows/stop-service.bat
        deploy/windows/restart-service.bat
        DESTINATION .)
elseif(UNIX AND NOT APPLE)
    install(PROGRAMS
        deploy/linux/service-common.sh
        deploy/linux/install-service.sh
        deploy/linux/uninstall-service.sh
        deploy/linux/start-service.sh
        deploy/linux/stop-service.sh
        deploy/linux/restart-service.sh
        DESTINATION .)
endif()

if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:lw-ppocr-http-service>
        DESTINATION . OPTIONAL)
endif()
