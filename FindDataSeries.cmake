# Find the DataSeries includes and library
#
#  DATASERIES_INCLUDE_DIR - where to find DataSeries/*.H
#  DATASERIES_LIBRARIES   - List of libraries when using DataSeries
#  DATASERIES_FOUND       - True if DataSeries found.


IF (DATASERIES_INCLUDE_DIR)
  # Already in cache, be silent
  SET(DATASERIES_FIND_QUIETLY TRUE)
ENDIF (DATASERIES_INCLUDE_DIR)

FIND_PATH(DATASERIES_INCLUDE_DIR DataSeries/Extent.H
  ${CMAKE_INSTALL_PREFIX}/include
)

FIND_LIBRARY(DATASERIES_LIBRARY 
  NAMES DataSeries 
  PATHS ${CMAKE_INSTALL_PREFIX}/lib)

IF (DATASERIES_INCLUDE_DIR AND DATASERIES_LIBRARY)
   SET(DATASERIES_FOUND TRUE)
   SET(DATASERIES_LIBRARIES ${DATASERIES_LIBRARY})
ELSE (DATASERIES_INCLUDE_DIR AND DATASERIES_LIBRARY)
   SET(DATASERIES_FOUND FALSE)
   SET(DATASERIES_LIBRARIES)
ENDIF (DATASERIES_INCLUDE_DIR AND DATASERIES_LIBRARY)

IF (DATASERIES_FOUND)
   IF (NOT DATASERIES_FIND_QUIETLY)
      MESSAGE(STATUS "Found DataSeries: ${DATASERIES_LIBRARY}")
   ENDIF (NOT DATASERIES_FIND_QUIETLY)
ELSE (DATASERIES_FOUND)
   IF (DATASERIES_FIND_REQUIRED)
      MESSAGE(STATUS "Looked for DataSeries libraries named DataSeries in ${CMAKE_INSTALL_PREFIX}/lib and system paths")
      MESSAGE(STATUS "got: DATASERIES_INCLUDE_DIR=${DATASERIES_INCLUDE_DIR}")
      MESSAGE(STATUS "got: DATASERIES_LIBRARY=${DATASERIES_LIBRARY}")
      MESSAGE(FATAL_ERROR "ERROR: Could NOT find DataSeries library")
   ENDIF (DATASERIES_FIND_REQUIRED)
ENDIF (DATASERIES_FOUND)

MARK_AS_ADVANCED(
  DATASERIES_LIBRARY
  DATASERIES_INCLUDE_DIR
)