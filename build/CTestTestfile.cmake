# CMake generated Testfile for 
# Source directory: /home/betty/repositories/zcoroutine
# Build directory: /home/betty/repositories/zcoroutine/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(fd_context_test "/home/betty/repositories/zcoroutine/build/fd_context_test")
set_tests_properties(fd_context_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(fiber_pool_test "/home/betty/repositories/zcoroutine/build/fiber_pool_test")
set_tests_properties(fiber_pool_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(fiber_test "/home/betty/repositories/zcoroutine/build/fiber_test")
set_tests_properties(fiber_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(scheduler_test "/home/betty/repositories/zcoroutine/build/scheduler_test")
set_tests_properties(scheduler_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(shared_stack_test "/home/betty/repositories/zcoroutine/build/shared_stack_test")
set_tests_properties(shared_stack_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(status_table_test "/home/betty/repositories/zcoroutine/build/status_table_test")
set_tests_properties(status_table_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(task_queue_test "/home/betty/repositories/zcoroutine/build/task_queue_test")
set_tests_properties(task_queue_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(timer_test "/home/betty/repositories/zcoroutine/build/timer_test")
set_tests_properties(timer_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;96;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(hook_integration_test "/home/betty/repositories/zcoroutine/build/hook_integration_test")
set_tests_properties(hook_integration_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;115;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(io_scheduler_integration_test "/home/betty/repositories/zcoroutine/build/io_scheduler_integration_test")
set_tests_properties(io_scheduler_integration_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;115;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(scheduler_fiber_integration_test "/home/betty/repositories/zcoroutine/build/scheduler_fiber_integration_test")
set_tests_properties(scheduler_fiber_integration_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;115;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
add_test(timer_scheduler_integration_test "/home/betty/repositories/zcoroutine/build/timer_scheduler_integration_test")
set_tests_properties(timer_scheduler_integration_test PROPERTIES  _BACKTRACE_TRIPLES "/home/betty/repositories/zcoroutine/CMakeLists.txt;115;add_test;/home/betty/repositories/zcoroutine/CMakeLists.txt;0;")
subdirs("zlog")
