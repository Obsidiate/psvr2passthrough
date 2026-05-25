# Regenerate the API layer manifest after build, replacing @LIBRARY_PATH@
# with the absolute path to the built DLL (forward-slashed for JSON safety).
file(READ "${SOURCE}" content)
file(TO_CMAKE_PATH "${LIBRARY_PATH}" library_path_json)
string(REPLACE "@LIBRARY_PATH@" "${library_path_json}" content "${content}")
file(WRITE "${DEST}" "${content}")
message(STATUS "Wrote ${DEST}")
