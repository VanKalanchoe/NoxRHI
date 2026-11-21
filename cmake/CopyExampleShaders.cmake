# Copies a directory's shaders next to the executables. Run at build time (cmake -P), so the file list is taken when the step runs: a
# .slang file added to an example is copied by the next build without re-running CMake (a configure-time file(GLOB) is not, when the
# IDE's build skips CMake's re-check). file(COPY) skips files that are already up to date. One step per source directory: two steps
# copying the same file into the same directory in parallel collide on Windows.
#   -DSOURCE_DIR=<examples/Name or examples/Common> -DDESTINATION=<exe dir>
file(GLOB shaders "${SOURCE_DIR}/*.slang")
file(COPY ${shaders} DESTINATION "${DESTINATION}")
