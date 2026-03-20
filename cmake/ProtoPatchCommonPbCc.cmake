# 由 code/Proto/CMakeLists.txt 调用：cmake -P ProtoPatchCommonPbCc.cmake <path/to/common.pb.cc>
# 对应原 regen_cpp.sh 中对 common.pb.cc 的 sed 修补（PROTOBUF_FIELD_OFFSET / offsetof）。
if(NOT DEFINED CMAKE_ARGV3)
  message(FATAL_ERROR "ProtoPatchCommonPbCc: usage: cmake -P ProtoPatchCommonPbCc.cmake <path/to/common.pb.cc>")
endif()
set(_f "${CMAKE_ARGV3}")
if(NOT EXISTS "${_f}")
  message(FATAL_ERROR "ProtoPatchCommonPbCc: file not found: ${_f}")
endif()
file(READ "${_f}" _c)
foreach(_m IN ITEMS
    TEXT_CONTENT PICTURE_CONTENT VOICE_CONTENT msg_content errorinfo user_info
    single_msg_push session_info quality_control_option taboo_option
  )
  string(REPLACE "PROTOBUF_FIELD_OFFSET(${_m}," "PROTOBUF_FIELD_OFFSET(::common::${_m}," _c "${_c}")
  string(REPLACE "offsetof(${_m}," "offsetof(::common::${_m}," _c "${_c}")
endforeach()
file(WRITE "${_f}" "${_c}")
