# Net: libNet.so（COMM 目录顶层源）+ Hello / Center 可执行文件（全量 src 下 *.cpp/*.cc）
set(NET_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

set(_libnet_dirs cmd step session labor protocol storage)
set(LIBNET_SOURCES "")
foreach(_d ${_libnet_dirs})
  file(GLOB _f "${NET_SRC}/${_d}/*.cpp" "${NET_SRC}/${_d}/*.cc")
  list(APPEND LIBNET_SOURCES ${_f})
endforeach()

file(GLOB_RECURSE NET_ALL_SOURCES CONFIGURE_DEPENDS
  "${NET_SRC}/*.cpp"
  "${NET_SRC}/*.cc"
)

add_library(Net SHARED ${LIBNET_SOURCES})
set_target_properties(Net PROPERTIES OUTPUT_NAME "Net")

add_executable(Hello ${NET_ALL_SOURCES})

foreach(_t Net Hello)
  thunder_apply_common_compile_options(${_t})
  thunder_target_include_net(${_t})
  thunder_link_thirdparty_shared(${_t})
endforeach()

target_link_options(Hello PRIVATE "LINKER:--export-dynamic")

if(THUNDER_BUILD_CENTER)
  add_executable(Center ${NET_ALL_SOURCES})
  target_compile_definitions(Center PRIVATE NODE_TYPE_CENTER)
  thunder_apply_common_compile_options(Center)
  thunder_target_include_net(Center)
  thunder_link_thirdparty_shared(Center)
  target_link_options(Center PRIVATE "LINKER:--export-dynamic")
endif()

if(THUNDER_DEPLOY_AUTO)
  include("${CMAKE_SOURCE_DIR}/cmake/ThunderDeploy.cmake")
  thunder_deploy_copy(Net "lib/libNet.so")
  thunder_deploy_copy(Hello "Hello/bin/Hello")
  if(THUNDER_BUILD_CENTER AND TARGET Center)
    # 与 deploy 脚本默认 CENTER_BIN=deploy/Center/bin/Center 一致
    thunder_deploy_copy(Center "Center/bin/Center")
  endif()
  # Logic/Interface 节点与 Hello 共用同一 Net 主程序；联调脚本默认 Logic/bin/Logic、Interface/bin/Interface
  add_custom_command(
    TARGET Hello
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${THUNDER_DEPLOY_ROOT}/Logic/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${THUNDER_DEPLOY_ROOT}/Interface/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Hello>"
            "${THUNDER_DEPLOY_ROOT}/Logic/bin/Logic"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Hello>"
            "${THUNDER_DEPLOY_ROOT}/Interface/bin/Interface"
    COMMENT "Deploy: deploy/Logic/bin/Logic + deploy/Interface/bin/Interface"
  )
endif()
