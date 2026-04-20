cmake_minimum_required(VERSION 3.16)

project(Motor_Drive_Frontend VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_VERSION_MAJOR 6)

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Widgets
    SerialPort
    PrintSupport
    Charts
)

qt_standard_project_setup()

qt_add_executable(Motor_Drive_Frontend
    main.cpp
    mainwindow.cpp
    mainwindow.ui
    mainwindow.h
    dataparser.cpp
    dataparser.h
    serialmanager.cpp
    serialmanager.h
    qcustomplot.cpp
    qcustomplot.h
    oscilloscopewidget.cpp
    oscilloscopewidget.h
    draglistwidget.cpp
    draglistwidget.h
    app_icon.rc
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::SerialPort
    Qt${QT_VERSION_MAJOR}::PrintSupport
    Qt${QT_VERSION_MAJOR}::Charts
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

target_include_directories(Motor_Drive_Frontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # 1. Search for windeployqt executable using Qt's qmake location
    get_target_property(_qt6_qmake_location Qt${QT_VERSION_MAJOR}::qmake LOCATION)
    get_filename_component(_qt6_bin_dir "${_qt6_qmake_location}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS "${_qt6_bin_dir}"
        REQUIRED
    )

    # 2. Obtain MinGW runtime DLL paths from the compiler location
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MINGW_RUNTIME_DLLS
        "${MINGW_BIN_DIR}/libgcc_s_seh-1.dll"
        "${MINGW_BIN_DIR}/libstdc++-6.dll"
        "${MINGW_BIN_DIR}/libwinpthread-1.dll"
    )

    # 3. Set the output directory for the packaged application
    set(PACKAGE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/out/build/release")

    # 4. Custom target to package the application with dependencies
    add_custom_target(package
        # Create the output directory if it doesn't exist
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_OUTPUT_DIR}"
        # Copy the built executable to the output directory
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${PACKAGE_OUTPUT_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        # Run windeployqt to copy Qt dependencies
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            "${PACKAGE_OUTPUT_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
            --dir "${PACKAGE_OUTPUT_DIR}"
            --no-compiler-runtime   # Do not automatically copy MinGW runtime, we will copy it manually for better reliability
            --release
        # Manually copy MinGW runtime libraries
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MINGW_RUNTIME_DLLS}
            "${PACKAGE_OUTPUT_DIR}/"
        COMMENT "打包完成：${PACKAGE_OUTPUT_DIR}"
        DEPENDS ${PROJECT_NAME}
        VERBATIM
    )
endif()

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
