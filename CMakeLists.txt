cmake_minimum_required(VERSION 3.22)
project(imgui_bundle_example_integration)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Release optimizations -------------------------------------------------
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    # Strip debug symbols, optimize for speed
    add_compile_options(-O2 -s)
    add_link_options(-O2 -s)
endif()

# ---- Disable unused imgui_bundle components --------------------------------
# These options must be set BEFORE FetchContent_MakeAvailable / add_subdirectory

set(HELLOIMGUI_EMBED_ASSETS ON CACHE BOOL "" FORCE)

# Demos & tests
set(IMGUI_BUNDLE_BUILD_DEMOS             OFF CACHE BOOL "" FORCE)

# Plotting / visualization
set(IMGUI_BUNDLE_WITH_IMPLOT             OFF CACHE BOOL "" FORCE)

# 3-D gizmo / transform editor
set(IMGUI_BUNDLE_WITH_IMGUIZMO           OFF CACHE BOOL "" FORCE)

# Node / graph editor
set(IMGUI_BUNDLE_WITH_IMGUI_NODE_EDITOR  OFF CACHE BOOL "" FORCE)

# Markdown rendering
set(IMGUI_BUNDLE_WITH_IMMARKDOWN         OFF CACHE BOOL "" FORCE)
set(IMGUI_BUNDLE_WITH_IMGUI_MD           OFF CACHE BOOL "" FORCE)

# Code / text editor widget
set(IMGUI_BUNDLE_WITH_IMGUI_COLOR_TEXT_EDIT OFF CACHE BOOL "" FORCE)

# Texture inspector
set(IMGUI_BUNDLE_WITH_IMGUI_TEX_INSPECT  OFF CACHE BOOL "" FORCE)

# Flame-graph profiler widget
set(IMGUI_BUNDLE_WITH_IMGUI_FLAME_GRAPH  OFF CACHE BOOL "" FORCE)

# Spinner widgets
set(IMGUI_BUNDLE_WITH_IMSPINNER          OFF CACHE BOOL "" FORCE)

# Knob widgets
set(IMGUI_BUNDLE_WITH_IMGUI_KNOBS        OFF CACHE BOOL "" FORCE)

# Gradient editor
set(IMGUI_BUNDLE_WITH_IMGUI_GRADIENT     OFF CACHE BOOL "" FORCE)

# Cool bar
set(IMGUI_BUNDLE_WITH_IMGUI_COOL_BAR     OFF CACHE BOOL "" FORCE)

# Toggle buttons
set(IMGUI_BUNDLE_WITH_IMGUI_TOGGLE       OFF CACHE BOOL "" FORCE)

# Command palette
set(IMGUI_BUNDLE_WITH_IMCMD              OFF CACHE BOOL "" FORCE)

# Portable file dialogs
set(IMGUI_BUNDLE_WITH_PORTABLE_FILE_DIALOGS OFF CACHE BOOL "" FORCE)

# Python bindings
set(IMGUI_BUNDLE_BUILD_PYTHON_BINDINGS   OFF CACHE BOOL "" FORCE)


# ---- Add imgui_bundle ------------------------------------------------------
if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui_bundle)
    add_subdirectory(external/imgui_bundle EXCLUDE_FROM_ALL)
endif()

if (NOT TARGET imgui_bundle)
    message(STATUS "Fetching imgui_bundle...")
    include(FetchContent)
    Set(FETCHCONTENT_QUIET FALSE)
    FetchContent_Declare(
        imgui_bundle
        GIT_REPOSITORY https://github.com/pthom/imgui_bundle.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(imgui_bundle)
endif()

# ---- Build app -------------------------------------------------------------
option(EXAMPLE_INTEGRATION_PORTABLE_INSTALL "Install in a portable way" ON)

if (EXAMPLE_INTEGRATION_PORTABLE_INSTALL)
    imgui_bundle_add_app(imgui_bundle_example_integration hello_world.cpp)
else()
    set(HELLOIMGUI_ADD_APP_WITH_INSTALL OFF CACHE BOOL "" FORCE)
    imgui_bundle_add_app(imgui_bundle_example_integration hello_world.cpp)

    include(GNUInstallDirs)
    install(TARGETS imgui_bundle_example_integration DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(assets_install_dir ${CMAKE_INSTALL_DATADIR}/imgui_bundle_example_integration)
    install(DIRECTORY assets DESTINATION ${assets_install_dir})

    file(RELATIVE_PATH assets_relative_to_bin
        "/${CMAKE_INSTALL_BINDIR}"
        "/${assets_install_dir}/assets")
    target_compile_definitions(imgui_bundle_example_integration PRIVATE ASSETS_LOCATION="${assets_relative_to_bin}")
endif()
