set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

add_executable(metal-smoke EXCLUDE_FROM_ALL main.mm)

target_compile_definitions(metal-smoke PRIVATE TOOLS)

target_include_directories(metal-smoke PRIVATE
	${CMAKE_SOURCE_DIR}/rts
	${CMAKE_BINARY_DIR}/src-generated/engine
)
