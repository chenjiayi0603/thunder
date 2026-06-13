# HelloHttp 插件: ModuleHello.so (HTTP 编解码)
if(NOT TARGET Net)
  message(FATAL_ERROR "HelloHttp requires target 'Net' from code/Net")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB MODULE_HELLO_SRCS CONFIGURE_DEPENDS "${_src}/ModuleHello/*.cpp")

add_library(ModuleHelloHttp SHARED
  ${MODULE_HELLO_SRCS}
  "${_src}/CustomLogger.cpp"
  "${_src}/HelloSession.cpp"
)
set_target_properties(ModuleHelloHttp PROPERTIES OUTPUT_NAME "HelloHttp_ModuleHello" PREFIX "")
target_compile_features(ModuleHelloHttp PRIVATE cxx_std_20)
target_compile_definitions(ModuleHelloHttp PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleHelloHttp PRIVATE "${_src}/ModuleHello" "${_src}")
thunder_plugin_include_base(ModuleHelloHttp)
thunder_plugin_link_net_proto_util(ModuleHelloHttp)

# ModuleRaw — 纯 echo, 零业务开销 (Fast Path)
file(GLOB MODULE_RAW_SRCS CONFIGURE_DEPENDS "${_src}/ModuleRaw/*.cpp")
add_library(ModuleRawHttp SHARED
  ${MODULE_RAW_SRCS}
  "${_src}/CustomLogger.cpp"
  "${_src}/HelloSession.cpp"
)
set_target_properties(ModuleRawHttp PROPERTIES OUTPUT_NAME "HelloHttp_ModuleRaw" PREFIX "")
target_compile_features(ModuleRawHttp PRIVATE cxx_std_20)
target_compile_definitions(ModuleRawHttp PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleRawHttp PRIVATE "${_src}/ModuleRaw" "${_src}")
thunder_plugin_include_base(ModuleRawHttp)
thunder_plugin_link_net_proto_util(ModuleRawHttp)

# ModuleLua — LuaJIT 脚本模块
file(GLOB MODULE_LUA_SRCS CONFIGURE_DEPENDS "${_src}/ModuleLua/*.cpp")
add_library(ModuleLuaHttp SHARED
  ${MODULE_LUA_SRCS}
  "${_src}/CustomLogger.cpp"
  "${_src}/HelloSession.cpp"
)
set_target_properties(ModuleLuaHttp PROPERTIES OUTPUT_NAME "HelloHttp_ModuleLua" PREFIX "")
target_compile_features(ModuleLuaHttp PRIVATE cxx_std_20)
target_compile_definitions(ModuleLuaHttp PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleLuaHttp PRIVATE "${_src}/ModuleLua" "${_src}" "/usr/include/luajit-2.1" "${CMAKE_SOURCE_DIR}/code/3party/include")
thunder_plugin_include_base(ModuleLuaHttp)
thunder_plugin_link_net_proto_util(ModuleLuaHttp)
target_link_libraries(ModuleLuaHttp PRIVATE "/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2")

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(ModuleHelloHttp "HelloHttp/plugins/HelloHttp_ModuleHello.so")
  thunder_deploy_copy(ModuleRawHttp "HelloHttp/plugins/HelloHttp_ModuleRaw.so")
endif()
