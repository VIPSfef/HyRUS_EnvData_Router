# Source archives contain no installed bin/ programs and need no mode fix.
if(CPACK_SOURCE_INSTALLED_DIRECTORIES)
    return()
endif()
# CPack's Windows archive backend otherwise records Linux ELF files as 0666.
# These variables are copied into CPackConfig.cmake by include(CPack).
foreach(package IN LISTS CPACK_PACKAGE_FILES)
    if(package MATCHES "\\.tar\\.gz$")
        execute_process(
            COMMAND "${CPACK_LINUX_ARCHIVE_PYTHON}" "${CPACK_LINUX_ARCHIVE_NORMALIZER}" "${package}"
            RESULT_VARIABLE normalize_result
        )
        if(NOT normalize_result EQUAL 0)
            message(FATAL_ERROR "Could not restore Linux package executable permissions")
        endif()
    endif()
endforeach()