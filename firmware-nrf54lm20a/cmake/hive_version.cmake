# hive_read_identity(<hive_config.h> <version-var> <board-var>)
#
# Reads the firmware version (MAJOR.MINOR.PATCH, from the three
# HIVEINSIDE_FW_VERSION_* defines) and HIVEINSIDE_BOARD out of
# src/hive_config.h, the version's single source of truth. Either output is
# empty when the header no longer matches the expected form.
function(hive_read_identity header version_var board_var)
  file(READ ${header} contents)

  set(parts "")
  foreach(component MAJOR MINOR PATCH)
    string(REGEX MATCH
      "define[ \t]+HIVEINSIDE_FW_VERSION_${component}[ \t]+([0-9]+)"
      matched "${contents}")
    if(matched)
      list(APPEND parts "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(LENGTH parts count)
  set(version "")
  if(count EQUAL 3)
    list(JOIN parts "." version)
  endif()

  string(REGEX MATCH
    "define[ \t]+HIVEINSIDE_BOARD[ \t]+\"([A-Za-z0-9_]+)\""
    matched "${contents}")
  set(board "")
  if(matched)
    set(board "${CMAKE_MATCH_1}")
  endif()

  set(${version_var} "${version}" PARENT_SCOPE)
  set(${board_var} "${board}" PARENT_SCOPE)
endfunction()
