cmake_minimum_required(VERSION 3.16)
project(Lights VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    resources.qrc
    # mainwindow.ui   # ← remove this line if you don’t have a .ui file yet
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Lights
        # MANUAL_FINALIZATION   # ← you can delete this line, it’s unnecessary for simple apps
        ${PROJECT_SOURCES}
    )
else()
    if(ANDROID)
        add_library(Lights SHARED ${PROJECT_SOURCES})
    else()
        add_executable(Lights ${PROJECT_SOURCES})
    endif()
endif()

# ←←← ALL target_link_libraries go HERE, AFTER the executable exists!
target_link_libraries(Lights PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

set_target_properties(Lights PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Lights)
endif()
