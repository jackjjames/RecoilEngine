set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

find_package(SDL2 MODULE REQUIRED)
include(FetchShaderTranslator)

add_executable(metal-smoke EXCLUDE_FROM_ALL
	main.mm
	log_stub.c
	${CMAKE_SOURCE_DIR}/rts/Rendering/MetalRenderGlobals.mm
	${CMAKE_SOURCE_DIR}/rts/Rendering/MetalBuffer.mm
	${CMAKE_SOURCE_DIR}/rts/Rendering/MetalShaderPipeline.mm
	${CMAKE_SOURCE_DIR}/rts/Rendering/Platform/MetalOffscreenFrame.mm
	${CMAKE_SOURCE_DIR}/rts/Rendering/ShaderTranslator.cpp
)

target_compile_definitions(metal-smoke PRIVATE TOOLS)

target_include_directories(metal-smoke PRIVATE
	${CMAKE_SOURCE_DIR}/rts
	${CMAKE_BINARY_DIR}/src-generated/engine
)

target_link_libraries(metal-smoke PRIVATE
	SDL2::SDL2
	${SPRING_SHADER_TRANSLATOR_LIBS}
	"-framework Metal"
	"-framework QuartzCore"
	"-framework Foundation"
	"-framework Cocoa"
)

set_target_properties(metal-smoke PROPERTIES
	OBJCXX_STANDARD 23
	OBJCXX_STANDARD_REQUIRED ON
)
