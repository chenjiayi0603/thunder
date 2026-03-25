# Hello 插件：ModuleHello.so（链接 libNet + libProto）
if(NOT TARGET Net)
  message(FATAL_ERROR "Hello plugins require target 'Net' from code/Net")
endif()
if(NOT TARGET Proto)
  message(FATAL_ERROR "Hello plugins require target 'Proto' from code/Proto")
endif()
if(NOT TARGET Util)
  message(FATAL_ERROR "Hello plugins require target 'Util' from code/Util")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

set(_mh_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/ModuleHello")
file(GLOB MODULE_HELLO_SRCS CONFIGURE_DEPENDS "${_mh_dir}/*.cpp")

set(_hello_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(ModuleHello SHARED
  ${MODULE_HELLO_SRCS}
  "${_hello_src}/CustomLogger.cpp"
  "${_hello_src}/HelloSession.cpp"
)

set_target_properties(ModuleHello PROPERTIES OUTPUT_NAME "ModuleHello" PREFIX "")

target_compile_features(ModuleHello PRIVATE cxx_std_20)
target_compile_definitions(ModuleHello PRIVATE _LINUX_OS_ NODE_BEAT=10.0)

target_include_directories(ModuleHello PRIVATE
  "${_mh_dir}"
  "${_hello_src}"
)
thunder_plugin_include_base(ModuleHello)
thunder_plugin_link_net_proto_util(ModuleHello)

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(ModuleHello "Hello/plugins/ModuleHello.so")
endif()

set(_cmd_hello_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/CmdHello")
add_library(CmdHello SHARED
  "${_cmd_hello_dir}/CmdHello.cpp"
  "${_hello_src}/CustomLogger.cpp"
  "${_hello_src}/HelloSession.cpp"
)
set_target_properties(CmdHello PROPERTIES OUTPUT_NAME "CmdHello" PREFIX "")
target_compile_features(CmdHello PRIVATE cxx_std_20)
target_compile_definitions(CmdHello PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(CmdHello PRIVATE
  "${_cmd_hello_dir}"
  "${_hello_src}"
)
thunder_plugin_include_base(CmdHello)
thunder_plugin_link_net_proto_util(CmdHello)
if(THUNDER_DEPLOY_AUTO)
  thunder_deploy_copy(CmdHello "Hello/plugins/CmdHello.so")
endif()

add_custom_target(HelloPlugins DEPENDS ModuleHello CmdHello)
