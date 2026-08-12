if (NOT DEFINED EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
    message(FATAL_ERROR "Windows server executable was not provided for manifest verification")
endif()

# Application manifests are embedded as text in the PE resource section. This
# catches linker/build regressions that silently fall back to the default
# asInvoker execution level, which would make elevated application audio silent
# again even though the server still builds successfully.
file(STRINGS "${EXECUTABLE}" ELEVATION_MARKERS
    REGEX "requireAdministrator"
    LIMIT_COUNT 1
)
if (NOT ELEVATION_MARKERS)
    message(FATAL_ERROR
        "${EXECUTABLE} does not contain a requireAdministrator UAC manifest"
    )
endif()

message(STATUS "Verified requireAdministrator UAC manifest in ${EXECUTABLE}")
