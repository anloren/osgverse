SET(EXE_NAME osgVerse_EarthExplorer)
SET(EXECUTABLE_FILES
    earth_main.cpp ui.cpp render_effects.cpp
    city_data.cpp ReaderWriterCSV.cpp quake_data.cpp precip_data.cpp flight_data.cpp tiles3d_data.cpp
    ai_chat.cpp ai_setup.cpp ai_ui.cpp ai_cards.cpp ai_media.cpp
)
ADD_DEFINITIONS(-DHV_STATICLIB)
INCLUDE_DIRECTORIES(../../3rdparty/libhv ../../3rdparty/libhv/all)

NEW_EXECUTABLE(${EXE_NAME} SHARED)
SET_PROPERTY(TARGET ${EXE_NAME} PROPERTY FOLDER "APPLICATIONS")
TARGET_LINK_LIBRARIES(${EXE_NAME} osgVerseReaderWriter osgVerseAnimation osgVersePipeline
                      osgVerseWrappers osgVerseModeling osgVerseScript osgVerseUI osgVerseAI
                      osgVerseDependency ${EXTERNAL_LIBRARIES})
LINK_OSG_LIBRARY(${EXE_NAME} OpenThreads osg osgDB osgUtil osgGA osgText osgSim osgTerrain osgViewer)
TARGET_COMPILE_OPTIONS(${EXE_NAME} PUBLIC -D_SCL_SECURE_NO_WARNINGS)

USE_MSVC_DEBUGGER(${EXE_NAME} "${THIRDPARTY_ROOT}/bin;${OSG_BUILD_INCLUDE_DIR}/../bin")
IF(MSVC AND VERSE_INSTALL_PDB_FILES)
    INSTALL(FILES $<TARGET_PDB_FILE:${EXE_NAME}> DESTINATION ${INSTALL_BINDIR} OPTIONAL)
ENDIF()
