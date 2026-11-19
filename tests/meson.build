#############################################################################
#
# Copyright (C) 2019 Collabora Ltd
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

check_dep = dependency('check', required : false)

# The render-ring tests are self-contained (the helpers are header-only)
# and do not need check, so they build regardless.
ring_tests = [
   ['test_render_ring', 'test_render_ring.c'],
]

foreach t : ring_tests
   test_ring = executable(t[0], t[1],
                          include_directories : include_directories('..'))
   test(t[0], test_ring)
endforeach

# The proxy child monitor is self-contained; test it directly.
if host_machine.system() == 'darwin'
   test_proxy_monitor = executable(
      'test_proxy_monitor',
      ['test_proxy_monitor.c', '../src/proxy/proxy_monitor.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa/compat'))
   test('test_proxy_monitor', test_proxy_monitor)

   test_socket_watcher = executable(
      'test_socket_watcher',
      'test_socket_watcher.c')
   test('test_socket_watcher', test_socket_watcher)
endif

test_npt_doorbell = executable(
   'test_npt_doorbell',
   ['test_npt_doorbell.c', '../src/proxy/proxy_doorbell.c'],
   include_directories : include_directories(
      '..', '../src', '../src/mesa/compat'))
test('test_npt_doorbell', test_npt_doorbell)

if check_dep.found()

libvrtest_sources = [
   'testvirgl.c',
   'testvirgl.h',
   'testvirgl_encode.c',
   'testvirgl_encode.h',
]

libvrtest_depends = [
   libvirgl_dep,
   gallium_dep,
   check_dep,
]

if with_tracing == 'percetto'
   libvrtest_depends += [percetto_dep]
endif

libvrtest = static_library(
   'vrtest',
   libvrtest_sources,
   dependencies : libvrtest_depends
)

tests = [
   ['test_virgl_init', 'test_virgl_init.c'],
   ['test_virgl_fence', 'test_virgl_fence.c'],
   ['test_virgl_resource', 'test_virgl_resource.c'],
   ['test_virgl_transfer', 'test_virgl_transfer.c'],
   ['test_virgl_cmd', 'test_virgl_cmd.c'],
   ['test_virgl_strbuf', 'test_virgl_strbuf.c']
]

fuzzy_tests = [
   ['test_fuzzer_formats', 'test_fuzzer_formats.c'],
]

test_depends = [
   libvirglrenderer_dep,
   check_dep,
]

if with_tracing == 'percetto'
   test_depends += [percetto_dep]
endif

foreach t : tests
   test_virgl = executable(t[0], t[1], link_with: libvrtest,
                           dependencies : test_depends)
   test(t[0], test_virgl)
endforeach

# For some unknown reason running this test in paralel with the others
# breaks some tests, so run it sequentially until we figure out what
# goes wrong
test_virgl_gbm_resources = executable(
   'test_virgl_gbm_resources',
   'test_virgl_gbm_resources.c',
   link_with: libvrtest,
   dependencies : test_depends)

test('test_virgl_gbm_resources', test_virgl_gbm_resources, is_parallel : false)

fuzzytest_depends = [
   libvirglrenderer_dep,
   epoxy_dep,
]

if with_tracing == 'percetto'
   fuzzytest_depends += [percetto_dep]
endif

foreach t : fuzzy_tests
   test_virgl_fuzzy = executable(t[0], t[1], link_with: libvrtest,
                                 dependencies : fuzzytest_depends)
   test(t[0], test_virgl)
endforeach


if with_valgrind
   valgrind = find_program('valgrind')
   surpression_path = join_paths(meson.current_source_dir(), 'valgrind.suppressions')
   args = ['--leak-check=full', '--quiet', '--error-exitcode=3',
           '--suppressions='+ surpression_path]
   foreach t : tests
      test('valgrind-' + t[0],
           valgrind,
           args : args + [join_paths(meson.current_build_dir(), t[0])],
           timeout : 1800)
   endforeach
endif

endif # check_dep.found()

if with_fuzzer
   subdir('fuzzer')
endif

if with_neptune
   neptune_test_depends = [
      libvirglrenderer_dep,
   ]

   neptune_test_inc = include_directories('../src')

   neptune_tests = [
      ['test_neptune_init', 'test_neptune_init.c'],
      ['test_neptune_async_create', 'test_neptune_async_create.c'],
      ['test_neptune_status_page', 'test_neptune_status_page.c'],
   ]

   # Neptune init forks the render server, so point at the in-tree
   # binary instead of the install-prefix path baked into config.h.
   neptune_test_env = environment()
   neptune_test_env.set('RENDER_SERVER_EXEC_PATH',
                        virgl_render_server.full_path())

   foreach t : neptune_tests
      exe = executable(t[0], t[1],
                       include_directories : neptune_test_inc,
                       dependencies : neptune_test_depends)
      test(t[0], exe, env : neptune_test_env)
   endforeach

   test_npt_completion = executable(
      'test_npt_completion',
      ['test_npt_completion.c', '../src/neptune/npt_completion.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include'))
   test('test_npt_completion', test_npt_completion)

   test_npt_queue_mode = executable(
      'test_npt_queue_mode',
      ['test_npt_queue_mode.c', '../src/neptune/npt_event.c',
       '../src/neptune/npt_event_fd.c',
       '../src/neptune/npt_queue.c', '../src/neptune/npt_completion.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include',
         '../subprojects/directx-native/include'))
   test('test_npt_queue_mode', test_npt_queue_mode)

   test_npt_feedback_fence = executable(
      'test_npt_feedback_fence',
      ['test_npt_feedback_fence.c', '../src/neptune/npt_feedback.c',
       '../src/neptune/npt_event_fd.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include',
         '../subprojects/directx-native/include'))
   test('test_npt_feedback_fence', test_npt_feedback_fence)

   test_npt_feedback_deferred = executable(
      'test_npt_feedback_deferred',
      ['test_npt_feedback_deferred.c', '../src/neptune/npt_feedback.c',
       '../src/neptune/npt_event_fd.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include',
         '../subprojects/directx-native/include'))
   test('test_npt_feedback_deferred', test_npt_feedback_deferred)

   test_npt_desc_batch = executable(
      'test_npt_desc_batch',
      ['test_npt_desc_batch.c', '../src/neptune/npt_desc_batch.c',
       '../src/neptune/npt_cs.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include',
         '../subprojects/directx-native/include'))
   test('test_npt_desc_batch', test_npt_desc_batch)

   test_npt_ring_decode = executable(
      'test_npt_ring_decode',
      ['test_npt_ring_decode.c', '../src/neptune/npt_ring.c',
       '../src/neptune/npt_cs.c'],
      include_directories : include_directories(
         '..', '../src', '../src/mesa', '../src/mesa/compat',
         '../src/neptune', '../src/gallium/auxiliary',
         '../src/gallium/include',
         '../subprojects/directx-native/include'))
   test('test_npt_ring_decode', test_npt_ring_decode)
endif

if with_venus
   test_venus_image_format = executable(
      'test_venus_image_format', 'test_venus_image_format.c',
      include_directories: include_directories('../src'))
   test('test_venus_image_format', test_venus_image_format)
endif
