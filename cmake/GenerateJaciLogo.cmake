# Generate an embedded C++ string from the canonical Jaci logo asset.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "Require INPUT and OUTPUT")
endif()

file(READ "${INPUT}" JACI_LOGO_CONTENT)
get_filename_component(JACI_LOGO_OUTPUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${JACI_LOGO_OUTPUT_DIR}")
file(WRITE "${OUTPUT}"
"// Generated from assets/ascii.txt. Do not edit.\n#pragma once\n\ninline constexpr const char* JaciAsciiLogo = R\"JACI_LOGO(${JACI_LOGO_CONTENT})JACI_LOGO\";\n")
