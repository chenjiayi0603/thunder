# 编译成功后复制到仓库根下 deploy/ 目录（路径均相对于项目根 ${CMAKE_SOURCE_DIR} 记录）
#
# 注意：add_custom_command(TARGET ...) 必须在「定义该 target 的 CMakeLists.txt」里调用，
# 因此 thunder_deploy_copy 只能写在 code/Proto、code/Net、code/Hello 等子目录中，不能写在根 CMakeLists.txt。
#
# 示例（相对仓库根）：
#   deploy/lib/libUtil.so
#   deploy/lib/libNet.so
#   deploy/lib/libProto.so
#   deploy/Hello/bin/Hello
#   deploy/bin/Center
#   deploy/Hello/plugins/ModuleHello.so
#
# 目标文件使用绝对路径执行 copy；COMMENT 中仅写 deploy/... 相对路径便于阅读与文档。

set(THUNDER_DEPLOY_ROOT "${CMAKE_SOURCE_DIR}/deploy" CACHE INTERNAL "")

function(thunder_deploy_copy _target _relpath_from_deploy_root)
  # _relpath_from_deploy_root 例如: lib/libNet.so、Hello/bin/Hello（勿前导 deploy/）
  set(_dest "${THUNDER_DEPLOY_ROOT}/${_relpath_from_deploy_root}")
  get_filename_component(_dest_dir "${_dest}" DIRECTORY)
  add_custom_command(
    TARGET ${_target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_target}>"
            "${_dest}"
    COMMENT "Deploy: deploy/${_relpath_from_deploy_root}"
  )
endfunction()
