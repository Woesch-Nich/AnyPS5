add_library(libSceAppContent SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceAppContent PRIVATE ${LIBS_INCLUDE_DIR})
# sceAppContentInitialize posts the entitlement event into libSceSystemService
# (cross-module NID import, same mechanism as libSceVideoOut -> libSceAgcDriver).
target_link_libraries(libSceAppContent PRIVATE libSceSystemService)
configure_windows_unwind(libSceAppContent)

if(BUILD_TESTING)
    add_test_executable(guest_appcontent_tests ${LIBS_INCLUDE_DIR}/tests/GuestAppContent.cpp)
    target_include_directories(guest_appcontent_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_appcontent_tests PRIVATE libSceAppContent libc)
    configure_windows_unwind(guest_appcontent_tests)
    add_test(NAME guest_appcontent COMMAND guest_appcontent_tests)
    if(WIN32)
        set_tests_properties(guest_appcontent PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAppContent>")
    endif()
endif()
