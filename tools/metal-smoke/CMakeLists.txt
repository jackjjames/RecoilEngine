set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

find_package(SDL2 MODULE REQUIRED)

add_executable(metal-smoke EXCLUDE_FROM_ALL
	main.mm
	${CMAKE_SOURCE_DIR}/rts/Rendering/MetalRenderGlobals.mm
)

target_compile_definitions(metal-smoke PRIVATE TOOLS)

target_include_directories(metal-smoke PRIVATE
	${CMAKE_SOURCE_DIR}/rts
	${CMAKE_BINARY_DIR}/src-generated/engine
)

target_link_libraries(metal-smoke PRIVATE
	SDL2::SDL2
	"-framework Metal"
	"-framework QuartzCore"
	"-framework Foundation"
	"-framework Cocoa"
)

set_target_properties(metal-smoke PROPERTIES
	OBJCXX_STANDARD 23
	OBJCXX_STANDARD_REQUIRED ON
)
