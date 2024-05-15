
set(PACK IPOPT)
set(${PACK}_FOUND TRUE)
set(${PACK}_INCLUDE_DIRS "/home/liujiangtao/osqp_ws/third_party/include/osqp")
file(GLOB ${PACK}_STATIC_LIBS  "/home/liujiangtao/osqp_ws/third_party/lib/*.a*")
file(GLOB ${PACK}_SHARED_LIBS  "/home/liujiangtao/osqp_ws/third_party/lib/*.so*")

set(${PACK}_LIBRARIES ${${PACK}_STATIC_LIBS} ${${PACK}_SHARED_LIBS})