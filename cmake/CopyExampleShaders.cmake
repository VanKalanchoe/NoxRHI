# Copies an example's shaders next to its executable. Run at build time (cmake -P), so the file list is taken when the example is built:
# a .slang file added to an example is copied by the next build without re-running CMake (a configure-time file(GLOB) is not, when
# the IDE's build skips CMake's re-check). file(COPY) skips files that are already up to date.
#   -DSOURCE_DIR=<examples/Name> -DCOMMON_FILE=<examples/Common/nox_gpu.slang> -DDESTINATION=<exe dir>
file(GLOB shaders "${SOURCE_DIR}/*.slang")
file(COPY ${shaders} "${COMMON_FILE}" DESTINATION "${DESTINATION}")
