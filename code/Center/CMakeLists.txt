# Center 节点插件（原 Center/src 下各 Cmd*/Module*/Makefile）
if(NOT THUNDER_BUILD_NODE_PLUGINS)
  return()
endif()
if(NOT TARGET Net OR NOT TARGET Proto OR NOT TARGET Util)
  message(FATAL_ERROR "Center plugins require Net, Proto, Util")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
endif()

set(_center_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(_session "${_center_src}/SessionOnlineNodes.cpp")

function(_center_plugin _name _subdir)
  add_library(${_name} SHARED
    "${_center_src}/${_subdir}/${_name}.cpp"
    "${_session}"
  )
  set_target_properties(${_name} PROPERTIES OUTPUT_NAME "${_name}" PREFIX "")
  target_compile_features(${_name} PRIVATE cxx_std_20)
  target_compile_definitions(${_name} PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
  target_include_directories(${_name} PRIVATE
    "${_center_src}/${_subdir}"
    "${_center_src}"
  )
  thunder_plugin_include_base(${_name}
    "${THUNDER_3PARTY}/include/mongoc/src/libbson/src"
    "${THUNDER_3PARTY}/include/mongoc/src/libbson/src/bson"
  )
  thunder_plugin_link_net_proto_util(${_name})
  if(THUNDER_DEPLOY_AUTO)
    thunder_deploy_copy(${_name} "Center/plugins/${_name}.so")
  endif()
endfunction()

_center_plugin(CmdElection CmdElection)
_center_plugin(CmdNodeDisconnect CmdNodeDisconnect)
_center_plugin(CmdNodeRegister CmdNodeRegister)
_center_plugin(CmdNodeReport CmdNodeReport)

add_library(ModuleAdmin SHARED
  "${_center_src}/ModuleAdmin/ModuleAdmin.cpp"
  "${_center_src}/ModuleAdmin/StepGetConfig.cpp"
  "${_center_src}/ModuleAdmin/StepSetConfig.cpp"
  "${_session}"
)
set_target_properties(ModuleAdmin PROPERTIES OUTPUT_NAME "ModuleAdmin" PREFIX "")
target_compile_features(ModuleAdmin PRIVATE cxx_std_20)
target_compile_definitions(ModuleAdmin PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleAdmin PRIVATE
  "${_center_src}/ModuleAdmin"
  "${_center_src}"
)
thunder_plugin_include_base(ModuleAdmin
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src"
  "${THUNDER_3PARTY}/include/mongoc/src/libbson/src/bson"
)
thunder_plugin_link_net_proto_util(ModuleAdmin)

if(THUNDER_DEPLOY_AUTO)
  thunder_deploy_copy(ModuleAdmin "Center/plugins/ModuleAdmin.so")
endif()
