# Logic 节点插件
if(NOT THUNDER_BUILD_NODE_PLUGINS)
  return()
endif()
if(NOT TARGET Net OR NOT TARGET Proto OR NOT TARGET Util)
  message(FATAL_ERROR "Logic plugins require Net, Proto, Util")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

add_library(CmdGetToken SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/src/CmdGetToken/CmdGetToken.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/LogicSession.cpp"
)
set_target_properties(CmdGetToken PROPERTIES OUTPUT_NAME "CmdGetToken" PREFIX "")

target_compile_features(CmdGetToken PRIVATE cxx_std_20)
target_compile_definitions(CmdGetToken PRIVATE _LINUX_OS_ NODE_BEAT=10.0)

target_include_directories(CmdGetToken PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/CmdGetToken"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
thunder_plugin_include_base(CmdGetToken)
thunder_plugin_link_net_proto_util(CmdGetToken)

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  # 与 deploy/Logic/conf 中 so_path「plugins/*.so」相对工作目录 deploy/Logic 一致
  thunder_deploy_copy(CmdGetToken "Logic/plugins/CmdGetToken.so")
endif()

# 聚合：本节点全部 .so 插件（新增插件时请把依赖加到此 target）
add_custom_target(LogicPlugins DEPENDS CmdGetToken)
