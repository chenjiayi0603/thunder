# HelloMqttBroker 插件: ModuleMqttBroker.so (MQTT 3.1.1 Broker)
if(NOT TARGET Net)
  message(FATAL_ERROR "HelloMqttBroker requires target 'Net' from code/Net")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/ThunderPlugin.cmake")

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB MODULE_MQTT_SRCS CONFIGURE_DEPENDS "${_src}/ModuleMqttBroker/*.cpp")

add_library(ModuleMqttBroker SHARED
  ${MODULE_MQTT_SRCS}
)
set_target_properties(ModuleMqttBroker PROPERTIES OUTPUT_NAME "HelloMqttBroker_ModuleMqttBroker" PREFIX "")
target_compile_features(ModuleMqttBroker PRIVATE cxx_std_20)
target_compile_definitions(ModuleMqttBroker PRIVATE _LINUX_OS_ NODE_BEAT=10.0)
target_include_directories(ModuleMqttBroker PRIVATE "${_src}/ModuleMqttBroker" "${_src}")
thunder_plugin_include_base(ModuleMqttBroker)
thunder_plugin_link_net_proto_util(ModuleMqttBroker)


if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(ModuleMqttBroker "MqttBroker/plugins/HelloMqttBroker_ModuleMqttBroker.so")
endif()
