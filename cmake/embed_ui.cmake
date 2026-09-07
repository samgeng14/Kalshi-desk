# Reads ui/index.html and writes a header with the page as a raw string literal.
file(READ "${IN}" CONTENT)
# Guard against the raw-string delimiter appearing inside the page.
string(FIND "${CONTENT}" ")DESKUI\"" HIT)
if(NOT HIT EQUAL -1)
  message(FATAL_ERROR "ui/index.html contains the raw string delimiter )DESKUI\"")
endif()
file(WRITE "${OUT}" "// generated from ui/index.html - do not edit\n#pragma once\nnamespace desk { inline const char* UI_HTML = R\"DESKUI(${CONTENT})DESKUI\"; }\n")
