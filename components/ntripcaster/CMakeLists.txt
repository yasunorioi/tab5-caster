# components/ntripcaster — cross-compiles the Zig ntripcaster (~/ntripcaster)
# into libntripcaster.a and links it into the firmware.
#
# The Zig backend files (io_lwip.zig / os_lwip.zig) @cImport FreeRTOS + lwIP +
# esp_timer headers, so we hand `zig build caster-lib` the same include set IDF
# gives its C components (every build component's public INCLUDE_DIRS + the
# sdkconfig.h dir + the newlib toolchain sysroot). The archive comes out
# riscv32 / RVC / ilp32f — the ABI esp-idf uses — so it links directly.

# caster_glue.c provides caster_console_write() (the log sink the Zig lib calls);
# caster.h declares that plus caster_start(). REQUIRES lists what the Zig
# @cImports pull in so their public headers land in the include set below.
idf_component_register(
    SRCS "caster_glue.c"
    INCLUDE_DIRS "."
    REQUIRES freertos lwip esp_timer esp_netif esp_common esp_system pthread
    # Force caster_glue.o (caster_console_write) into the link: the Zig lib is
    # the only referrer and is processed after this archive, so without
    # WHOLE_ARCHIVE the glue object is dropped and the symbol goes unresolved.
    WHOLE_ARCHIVE
)

set(NTRIPCASTER_DIR "$ENV{HOME}/ntripcaster")

# ── assemble the include set for zig's @cImport ──────────────────────────────
# Union of every build component's public include dirs (this is the same set the
# C compiler sees, resolved transitively by the component manager).
idf_build_get_property(_all_comps BUILD_COMPONENTS)
set(_zig_incs "")
foreach(_c ${_all_comps})
    idf_component_get_property(_cdir ${_c} COMPONENT_DIR)
    idf_component_get_property(_cinc ${_c} INCLUDE_DIRS)
    foreach(_d ${_cinc})
        if(IS_ABSOLUTE "${_d}")
            list(APPEND _zig_incs "${_d}")
        else()
            list(APPEND _zig_incs "${_cdir}/${_d}")
        endif()
    endforeach()
endforeach()
# sdkconfig.h (generated) + the newlib sysroot include (compiler builtin — not a
# component, derived from the cross-gcc path).
list(APPEND _zig_incs "${CMAKE_BINARY_DIR}/config")
get_filename_component(_tc_bindir "${CMAKE_C_COMPILER}" DIRECTORY)
list(APPEND _zig_incs "${_tc_bindir}/../riscv32-esp-elf/include")
list(REMOVE_DUPLICATES _zig_incs)

# ── build the static lib ─────────────────────────────────────────────────────
set(CASTER_LIB "${CMAKE_CURRENT_BINARY_DIR}/zig-out/lib/libntripcaster.a")
# Match esp32p4's ISA/ABI: rv32imafc, single-float (ilp32f).
set(ZIG_CPU "generic_rv32+m+a+f+c")

# The include list can be long; pass it via env (';'-joined) so it survives as a
# single argument. build.zig splits NTRIPCASTER_IDF_INCLUDES on ';'.
string(REPLACE ";" "\;" _zig_incs_env "${_zig_incs}")

add_custom_command(
    OUTPUT ${CASTER_LIB}
    COMMAND ${CMAKE_COMMAND} -E env
            "NTRIPCASTER_IDF_INCLUDES=${_zig_incs_env}"
            zig build caster-lib
                -Dio-backend=lwip
                -Dtarget=riscv32-freestanding
                -Dcpu=${ZIG_CPU}
                -Doptimize=ReleaseSmall
                --prefix "${CMAKE_CURRENT_BINARY_DIR}/zig-out"
                --cache-dir "${CMAKE_CURRENT_BINARY_DIR}/zig-cache"
    WORKING_DIRECTORY "${NTRIPCASTER_DIR}"
    COMMENT "Cross-compiling Zig ntripcaster for esp32p4"
    VERBATIM
    USES_TERMINAL
)
add_custom_target(ntripcaster_zig DEPENDS ${CASTER_LIB})
add_dependencies(${COMPONENT_LIB} ntripcaster_zig)
# Link the archive into the firmware. INTERFACE: the Zig lib has no headers of
# its own beyond caster.h (a plain C decl), so nothing to propagate but the .a.
target_link_libraries(${COMPONENT_LIB} INTERFACE ${CASTER_LIB})
