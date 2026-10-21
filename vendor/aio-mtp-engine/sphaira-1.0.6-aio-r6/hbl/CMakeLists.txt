project(hbl
    VERSION 3.0.0
    LANGUAGES ASM C
)

add_executable(hbl
    source/main.c
    source/trampoline.s
)

set_target_properties(hbl PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
)

target_link_options(hbl PRIVATE
    -Wl,-wrap,exit
)

target_compile_definitions(hbl PRIVATE
    VERSION="${CMAKE_PROJECT_VERSION}"
)

find_program(NX_ELF2NSO_EXE NAMES elf2nso HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(NX_NPDMTOOL_EXE NAMES npdmtool HINTS "${DEVKITPRO}/tools/bin" REQUIRED)

function(nx_create_nso target)
    cmake_parse_arguments(PARSE_ARGV 1 NX_NSO "" "OUTPUT" "")

    set(intarget "${target}")
    set(outtarget "${target}_nso")

    if (DEFINED NX_NSO_OUTPUT)
		get_filename_component(NX_NSO_OUTPUT "${NX_NSO_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
	else()
        set(NX_NSO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}.nso")
	endif()

    add_custom_command(
        OUTPUT "${NX_NSO_OUTPUT}"
        COMMAND ${NX_ELF2NSO_EXE} "$<TARGET_FILE:${intarget}>" "${NX_NSO_OUTPUT}"
        DEPENDS ${intarget}
        COMMENT "Converting ${intarget} to NSO format"
        VERBATIM
    )

    add_custom_target(${outtarget} ALL DEPENDS "${NX_NSO_OUTPUT}")
    dkp_set_target_file(${outtarget} "${NX_NSO_OUTPUT}")
endfunction()

function(nx_create_npdm target config)
    cmake_parse_arguments(PARSE_ARGV 1 NX_NPDM "" "OUTPUT;CONFIG" "")

    set(intarget "${target}")
    set(outtarget "${target}_npdm")

    if (DEFINED NX_NPDM_CONFIG)
		get_filename_component(NX_NPDM_CONFIG "${NX_NPDM_CONFIG}" ABSOLUTE)
	else()
		message(FATAL_ERROR "nx_create_exefs: must provide a CONFIG file in json format")
	endif()

    if (DEFINED NX_NPDM_OUTPUT)
		get_filename_component(NX_NPDM_OUTPUT "${NX_NPDM_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
	else()
        set(NX_NPDM_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}.npdm")
	endif()

    add_custom_command(
        OUTPUT "${NX_NPDM_OUTPUT}"
        COMMAND ${NX_NPDMTOOL_EXE} "${NX_NPDM_CONFIG}" "${NX_NPDM_OUTPUT}"
        DEPENDS "${NX_NPDM_CONFIG}"
        COMMENT "Generating NPDM for ${outtarget}"
        VERBATIM
    )

    add_custom_target(${outtarget} ALL DEPENDS "${NX_NPDM_OUTPUT}")
    dkp_set_target_file(${outtarget} "${NX_NPDM_OUTPUT}")
endfunction()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/exefs)

nx_create_nso(hbl
    OUTPUT exefs/main
)

nx_create_npdm(hbl
    OUTPUT exefs/main.npdm
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/hbl.json
)
