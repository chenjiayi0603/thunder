# HelloWs 插件: CmdHello + ModuleShake (WebSocket 编解码)
if(NOT TARGET Net)
  message(FATAL_ERROR "HelloWs requires target 'Net' from code/Net")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")
set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src")

# CmdHello
add_library(CmdHelloWs SHARED
  "${_src}/CmdHello/CmdHello.cpp"
  "${_src}/HelloSession.cpp"
)
set_target_properties(CmdHelloWs PROPERTIES OUTPUT_NAME "HelloWs_CmdHello" PREFIX "")
target_compile_features(CmdHelloWs PRIVATE cxx_std_20)
target_compile_definitions(CmdHelloWs PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(CmdHelloWs PRIVATE "${_src}/CmdHello" "${_src}")
thunder_plugin_include_base(CmdHelloWs)
thunder_plugin_link_net_proto_util(CmdHelloWs)

# ModuleShake
set(_ms_dir "${_src}/ModuleShake")
add_library(ModuleShakeWs SHARED "${_ms_dir}/ModuleShake.cpp" "${_src}/HelloSession.cpp")
set_target_properties(ModuleShakeWs PROPERTIES OUTPUT_NAME "HelloWs_ModuleShake" PREFIX "")
target_compile_features(ModuleShakeWs PRIVATE cxx_std_20)
target_compile_definitions(ModuleShakeWs PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleShakeWs PRIVATE "${_ms_dir}" "${_src}" "${THUNDER_3PARTY}" "${THUNDER_3PARTY}/cryptopp")
thunder_plugin_include_base(ModuleShakeWs)
thunder_plugin_link_net_proto_util(ModuleShakeWs)

# deploy
if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(CmdHelloWs "HelloWs/plugins/HelloWs_CmdHello.so")
  thunder_deploy_copy(ModuleShakeWs "HelloWs/plugins/HelloWs_ModuleShake.so")
endif()
