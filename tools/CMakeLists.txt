if(WIVRN_BUILD_DISSECTOR)
	add_subdirectory(wireshark)
endif()

if (WIVRN_BUILD_WIVRNCTL)
	add_subdirectory(wivrnctl)
endif()

if (WIN32 AND WIVRN_BUILD_SERVER)
	# Fake headset for testing the Windows server and its encoders without a headset (docs/windows.md)
	add_executable(wivrn-handshake-probe EXCLUDE_FROM_ALL handshake_probe/handshake_probe.cpp)
	target_link_libraries(wivrn-handshake-probe PRIVATE wivrn-common xrt-external-openxr)
	set_target_properties(wivrn-handshake-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/server)
endif()
