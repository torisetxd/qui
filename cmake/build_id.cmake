# Writes the checkout's commit, marked -dirty when it has local changes, to
# OUT as QUI_BUILD_ID, touching the file only when the id changes.
execute_process(COMMAND "${GIT}" -C "${SRC}" describe --always --dirty --abbrev=10
  OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed OR id STREQUAL "")
  set(id "unknown")
endif()
set(text "#define QUI_BUILD_ID \"${id}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
