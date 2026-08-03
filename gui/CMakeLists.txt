set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(GRAINMETER_GUI_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    resources.qrc
)

# Windows: embed the .ico so grainmeter-gui.exe shows the app icon in
# Explorer/taskbar/Alt-Tab, via the resource script referencing it.
if(WIN32)
    list(APPEND GRAINMETER_GUI_SOURCES grainmeter.rc)
	add_executable(grainmeter-gui WIN32 ${GRAINMETER_GUI_SOURCES})
else()
	add_executable(grainmeter-gui ${GRAINMETER_GUI_SOURCES})
endif()

target_link_libraries(grainmeter-gui PRIVATE Qt6::Widgets)

if(APPLE)
    # macOS needs an .icns, which (unlike the other formats) can only be
    # built with Apple's own `iconutil` -- there's no cross-platform way to
    # produce one, so it isn't generated automatically as part of this
    # build. One-time setup, on a Mac:
    #   iconutil -c icns assets/AppIcon.iconset -o assets/AppIcon.icns
    # (assets/AppIcon.iconset is already provided, with all the sizes
    # iconutil expects.) Once that file exists, this picks it up
    # automatically; until then, the app just builds with the default Qt
    # icon, so this is never a hard requirement.
    set(GRAINMETER_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/../assets/AppIcon.icns")
    if(EXISTS "${GRAINMETER_ICNS}")
        set_source_files_properties(${GRAINMETER_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(grainmeter-gui PRIVATE ${GRAINMETER_ICNS})
        set_target_properties(grainmeter-gui PROPERTIES MACOSX_BUNDLE_ICON_FILE "AppIcon.icns")
        message(STATUS "App icon: ${GRAINMETER_ICNS} found, will be bundled")
    else()
        message(STATUS "App icon: ${GRAINMETER_ICNS} not found -- run "
                        "'iconutil -c icns assets/AppIcon.iconset -o assets/AppIcon.icns' "
                        "to enable it (optional)")
    endif()

    set_target_properties(grainmeter-gui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Grainmeter"
    )
endif()
