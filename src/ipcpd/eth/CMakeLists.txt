get_filename_component(CURRENT_SOURCE_PARENT_DIR
  ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
get_filename_component(CURRENT_BINARY_PARENT_DIR
  ${CMAKE_CURRENT_BINARY_DIR} DIRECTORY)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

include_directories(${CURRENT_SOURCE_PARENT_DIR})
include_directories(${CURRENT_BINARY_PARENT_DIR})

include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_BINARY_DIR}/include)

find_path(NETMAP_C_INCLUDE_DIR
  net/netmap_user.h
  HINTS /usr/include /usr/local/include
)

mark_as_advanced(NETMAP_C_INCLUDE_DIR)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DISABLE_RAW_SOCKETS FALSE CACHE BOOL
    "Disable raw socket support for Ethernet IPCPs")
  if (NOT DISABLE_RAW_SOCKETS)
    message(STATUS "Raw socket support for Ethernet IPCPs enabled")
    set(HAVE_RAW_SOCKETS TRUE PARENT_SCOPE)
    set(HAVE_ETH TRUE)
  else ()
    message(STATUS "Raw socket support for Ethernet IPCPs disabled by user")
    unset(HAVE_RAW_SOCKETS)
    unset(HAVE_ETH)
  endif ()
endif ()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_path(BPF_C_INCLUDE_DIR
            net/bpf.h
            HINTS /usr/include /usr/local/include
  )

  mark_as_advanced(BPF_C_INCLUDE_DIR)

  if (BPF_C_INCLUDE_DIR)
    set(DISABLE_BPF FALSE CACHE BOOL
      "Disable Berkeley Packet Filter support for Ethernet IPCPs")
    if (NOT DISABLE_BPF)
      message(STATUS "Berkeley Packet Filter support "
                     "for Ethernet IPCPs enabled")
      set(HAVE_BPF TRUE PARENT_SCOPE)
      set(HAVE_ETH TRUE)
    else ()
      message(STATUS "Berkeley Packet Filter support "
                     "for Ethernet IPCPs disabled by user")
      unset(HAVE_BPF)
      unset(HAVE_ETH)
    endif ()
  endif ()
endif ()

if (NETMAP_C_INCLUDE_DIR)
  set(DISABLE_NETMAP FALSE CACHE BOOL
      "Disable netmap support for ETH IPCPs")
  if (NOT DISABLE_NETMAP)
    message(STATUS "Netmap support for Ethernet IPCPs enabled")
    set(HAVE_NETMAP TRUE PARENT_SCOPE)
    test_and_set_c_compiler_flag_global(-std=c99)
    set(HAVE_ETH TRUE)
  else ()
    message(STATUS "Netmap support for Ethernet IPCPs disabled by user")
    unset(HAVE_NETMAP)
    unset(HAVE_ETH)
    unset(IPCP_ETH_TARGET CACHE)
  endif ()
endif ()

if (HAVE_ETH)
  message(STATUS "Supported raw packet API found, building eth-llc and eth-dix")

  set(ETH_LLC_SOURCES
    # Add source files here
    ${CMAKE_CURRENT_SOURCE_DIR}/llc.c
  )

  set(ETH_DIX_SOURCES
    # Add source files here
    ${CMAKE_CURRENT_SOURCE_DIR}/dix.c
  )

  set(IPCP_ETH_LLC_TARGET ipcpd-eth-llc CACHE INTERNAL "")
  set(IPCP_ETH_DIX_TARGET ipcpd-eth-dix CACHE INTERNAL "")

  add_executable(ipcpd-eth-llc ${ETH_LLC_SOURCES} ${IPCP_SOURCES})
  add_executable(ipcpd-eth-dix ${ETH_DIX_SOURCES} ${IPCP_SOURCES})

  if (HAVE_BPF AND NOT APPLE)
    target_include_directories(ipcpd-eth-llc PUBLIC ${BPF_C_INCLUDE_DIR})
    target_include_directories(ipcpd-eth-dix PUBLIC ${BPF_C_INCLUDE_DIR})
  endif ()

  if (HAVE_NETMAP AND NOT APPLE)
    target_include_directories(ipcpd-eth-llc PUBLIC
      ${NETMAP_C_INCLUDE_DIR})
    target_include_directories(ipcpd-eth-dix PUBLIC
      ${NETMAP_C_INCLUDE_DIR})
  endif ()

  target_link_libraries(ipcpd-eth-llc LINK_PUBLIC ouroboros-dev)
  target_link_libraries(ipcpd-eth-dix LINK_PUBLIC ouroboros-dev)

  include(AddCompileFlags)
  if (CMAKE_BUILD_TYPE MATCHES "Debug*")
    add_compile_flags(ipcpd-eth-llc -DCONFIG_OUROBOROS_DEBUG)
    add_compile_flags(ipcpd-eth-dix -DCONFIG_OUROBOROS_DEBUG)
  endif ()

  install(TARGETS ipcpd-eth-llc ipcpd-eth-dix RUNTIME DESTINATION
    ${CMAKE_INSTALL_SBINDIR})
endif ()