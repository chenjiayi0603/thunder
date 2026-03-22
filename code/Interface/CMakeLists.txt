# Interface 节点插件
if(NOT THUNDER_BUILD_NODE_PLUGINS)
  return()
endif()
if(NOT TARGET Net OR NOT TARGET Proto OR NOT TARGET Util)
  message(FATAL_ERROR "Interface plugins require Net, Proto, Util")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

# 各节点插件独立编译：源文件均在 code/Interface/ 下，不引用 Hello/Logic/Center 的 .cpp
add_library(ModuleInterface SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ModuleHello/ModuleInterface.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/InterfaceSession.cpp"
)
set_target_properties(ModuleInterface PROPERTIES OUTPUT_NAME "ModuleInterface" PREFIX "")

target_compile_features(ModuleInterface PRIVATE cxx_std_20)
target_compile_definitions(ModuleInterface PRIVATE _LINUX_OS_ NODE_BEAT=10.0)

target_include_directories(ModuleInterface PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ModuleHello"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
thunder_plugin_include_base(ModuleInterface
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src"
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src/bson"
)
thunder_plugin_link_net_proto_util(ModuleInterface)

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  # 与 deploy/Interface/conf 中 so_path「plugins/*.so」相对工作目录 deploy/Interface 一致
  thunder_deploy_copy(ModuleInterface "Interface/plugins/ModuleInterface.so")
endif()

add_custom_target(InterfacePlugins DEPENDS ModuleInterface)
