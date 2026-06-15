# HelloHttps 插件: ModuleHello.so (HTTPS 编解码)
if(NOT TARGET Net)
  message(FATAL_ERROR "HelloHttps requires target 'Net' from code/Net")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB MODULE_HELLO_SRCS CONFIGURE_DEPENDS "${_src}/ModuleHello/*.cpp")

add_library(ModuleHelloHttps SHARED
  ${MODULE_HELLO_SRCS}
)
set_target_properties(ModuleHelloHttps PROPERTIES OUTPUT_NAME "HelloHttps_ModuleHello" PREFIX "")
target_compile_features(ModuleHelloHttps PRIVATE cxx_std_20)
target_compile_definitions(ModuleHelloHttps PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleHelloHttps PRIVATE "${_src}/ModuleHello" "${_src}")
thunder_plugin_include_base(ModuleHelloHttps)
thunder_plugin_link_net_proto_util(ModuleHelloHttps)

set(_raw_src "${CMAKE_CURRENT_SOURCE_DIR}/../HelloHttp/src/ModuleRaw")
add_library(ModuleRawHttps SHARED "${_raw_src}/ModuleRaw.cpp")
set_target_properties(ModuleRawHttps PROPERTIES OUTPUT_NAME "HelloHttps_ModuleRaw" PREFIX "")
target_compile_features(ModuleRawHttps PRIVATE cxx_std_20)
target_compile_definitions(ModuleRawHttps PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleRawHttps PRIVATE "${_raw_src}")
thunder_plugin_include_base(ModuleRawHttps)
thunder_plugin_link_net_proto_util(ModuleRawHttps)

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(ModuleHelloHttps "HelloHttps/plugins/HelloHttps_ModuleHello.so")
  thunder_deploy_copy(ModuleRawHttps   "HelloHttps/plugins/HelloHttps_ModuleRaw.so")
endif()
