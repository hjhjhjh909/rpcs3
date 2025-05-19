cmake_minimum_required(VERSION 3.14)

project(T500RS_Emulator)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
find_package(Threads REQUIRED)

# Add Linux-specific dependencies
if(UNIX AND NOT APPLE)
    find_package(Libusb REQUIRED)
    find_package(Libevdev REQUIRED)
    find_package(Udev REQUIRED)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/Emu/Io)

# Source files for the T500RS emulator
set(EMULATOR_SOURCES
    src/t500rs_emulator.cpp
    src/wheel_controller.cpp
    src/force_feedback.cpp
    rpcs3/Emu/Io/ThrustmasterT500RS.cpp
    rpcs3/Emu/Io/ThrustmasterT500RSConfig.cpp
)

# Source files for the UI
set(UI_SOURCES
    src/main_window.cpp
    src/settings_dialog.cpp
    src/qt_t500_ui.cpp
    rpcs3/rpcs3qt/emulated_thrustmaster_t500rs_settings_dialog.cpp
)

# Add executable for the emulator
add_executable(t500rs_emulator ${EMULATOR_SOURCES})
target_link_libraries(t500rs_emulator PRIVATE Threads::Threads)

# Add executable for the UI
add_executable(t500rs_ui ${UI_SOURCES})
target_link_libraries(t500rs_ui PRIVATE 
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Optional: Add resources if you have any
# qt_add_resources(QT_RESOURCES resources.qrc)
# target_sources(t500rs_ui PRIVATE ${QT_RESOURCES})

# Install targets
install(TARGETS t500rs_emulator DESTINATION bin)
install(TARGETS t500rs_ui DESTINATION bin)

if(UNIX AND NOT APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -pthread")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")
endif()
