cmake_minimum_required(VERSION 3.16)
project(Lights VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Multimedia)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Multimedia)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    resources.qrc
    infraredpanel.cpp
    infraredpanel.h
    broadlink.qrc
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Lights
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
    Qt${QT_VERSION_MAJOR}::Multimedia
)

set_target_properties(Lights PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_compile_definitions(Lights PRIVATE LIGHTS_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS Lights RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(Lights.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/lights.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lights.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/lightbulb.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME lights.svg)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(infraredpanel_test tests/infraredpanel_test.cpp infraredpanel.cpp infraredpanel.h broadlink.qrc)
    target_include_directories(infraredpanel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(infraredpanel_test PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME infrared_panel COMMAND infraredpanel_test)
    set_tests_properties(infrared_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME broadlink_bridge COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_broadlink_bridge.py)
endif()
