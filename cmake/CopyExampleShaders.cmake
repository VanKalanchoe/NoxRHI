# Copies an example's shaders next to its executable. Run at build time (cmake -P), so the file list is taken when the example is built:
# a .slang file added to an example is copied by the next build without re-running CMake (a configure-time file(GLOB) is not, when
# the IDE's build skips CMake's re-check). file(COPY) skips files that are already up to date.
#   -DSOURCE_DIR=<examples/Name> -DCOMMON_DIR=<examples/Common> -DDESTINATION=<exe dir>
# (examples/Common's .slang files too: nox_gpu.slang and nox_checks_selftest.slang)
file(GLOB shaders "${SOURCE_DIR}/*.slang" "${COMMON_DIR}/*.slang")
file(COPY ${shaders} DESTINATION "${DESTINATION}")
