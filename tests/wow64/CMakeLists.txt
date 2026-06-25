# ABI layout guard for include/pipeasio_unix_abi.h.
find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
    add_test(NAME wow64_unix_abi_layout
             COMMAND ${CMAKE_COMMAND} -E env
                     "CC=${CMAKE_C_COMPILER}"
                     "SRC=${CMAKE_CURRENT_SOURCE_DIR}/unix_abi_layout.c"
                     "INC=${CMAKE_SOURCE_DIR}/include"
                     ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_abi.sh)
    set_tests_properties(wow64_unix_abi_layout PROPERTIES TIMEOUT 60 LABELS "wow64;abi")
else()
    message(WARNING "bash not found - skipping the WoW64 ABI layout test.")
endif()
