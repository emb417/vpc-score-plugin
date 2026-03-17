cmake_minimum_required(VERSION 3.20)
project(VpcScorePlugin)

# Inject version from git tag or fallback
if(NOT DEFINED PLUGIN_VERSION)
   set(PLUGIN_VERSION "0.0.0")
endif()
target_compile_definitions(VpcScorePlugin PRIVATE PLUGIN_VERSION="${PLUGIN_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 99)

set(PLUGIN_SOURCES
   plugins/vpc-score-plugin/vpc-score-plugin.cpp
)

if(BUILD_SHARED OR NOT DEFINED BUILD_SHARED)
   add_library(VpcScorePlugin MODULE ${PLUGIN_SOURCES})

   target_include_directories(VpcScorePlugin PUBLIC
      plugins/vpc-score-plugin/include
   )

   if(WIN32 OR PluginPlatform STREQUAL "windows")
      target_compile_options(VpcScorePlugin PRIVATE
         $<$<CONFIG:Release>:/O2 /Oi /Ot /GF /GS- /Gy /GL>
      )
      target_link_options(VpcScorePlugin PRIVATE
         $<$<CONFIG:Release>:/INCREMENTAL:NO /OPT:REF /OPT:ICF /LTCG>
      )
      set_target_properties(VpcScorePlugin PROPERTIES
         MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      )
      if(CMAKE_SIZEOF_VOID_P EQUAL 8 OR PluginArch STREQUAL "x64")
         set_target_properties(VpcScorePlugin PROPERTIES OUTPUT_NAME "plugin-vpc-score64")
      else()
         set_target_properties(VpcScorePlugin PROPERTIES OUTPUT_NAME "plugin-vpc-score")
      endif()
   else()
      set_target_properties(VpcScorePlugin PROPERTIES
         PREFIX ""
         OUTPUT_NAME "plugin-vpc-score"
      )
      if(APPLE OR PluginPlatform STREQUAL "macos")
         set_target_properties(VpcScorePlugin PROPERTIES SUFFIX ".dylib")
      endif()
   endif()

   # Copy plugin.cfg and nvram-maps to output directory after build
   set(PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins" CACHE PATH "Plugin output directory")
   add_custom_command(TARGET VpcScorePlugin POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${PLUGINS_DIR}/vpc-score-plugin"
      COMMAND "${CMAKE_COMMAND}" -E copy
         "${CMAKE_SOURCE_DIR}/plugins/vpc-score-plugin/plugin.cfg"
         "${PLUGINS_DIR}/vpc-score-plugin/plugin.cfg"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
         "${CMAKE_SOURCE_DIR}/nvram-maps"
         "${PLUGINS_DIR}/vpc-score-plugin/nvram-maps"
      COMMAND "${CMAKE_COMMAND}" -E copy
         "$<TARGET_FILE:VpcScorePlugin>"
         "${PLUGINS_DIR}/vpc-score-plugin"
   )
endif()