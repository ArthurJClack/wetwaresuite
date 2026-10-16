# Small helpers the system tools (calc, code, explorer, terminal, monitor, clock)
# share: UTF-8 paths, whole-file reads and writes, the per-user folder, sizes.
add_library(wetwaresuite_sysutil INTERFACE)
add_library(wetwaresuite::sysutil ALIAS wetwaresuite_sysutil)
target_include_directories(wetwaresuite_sysutil INTERFACE include)
target_link_libraries(wetwaresuite_sysutil INTERFACE wetwaresuite::core)
