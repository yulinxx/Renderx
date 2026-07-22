# Renderx 渲染引擎单元测试配置

find_package(GTest REQUIRED)
include(GoogleTest)

file(GLOB_RECURSE RENDERX_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(RenderxTests ${RENDERX_TEST_SOURCES})

target_compile_features(RenderxTests PRIVATE cxx_std_17)

target_include_directories(RenderxTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(RenderxTests PRIVATE
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(RenderxTests)

set_property(TARGET RenderxTests PROPERTY FOLDER "Tests")

if(MSVC)
    target_compile_options(RenderxTests PRIVATE /utf-8)
endif()
