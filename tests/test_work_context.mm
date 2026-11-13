/* SPDX-License-Identifier: MIT
 * Run with test_work_context.py on macOS. Uses the production control plane and
 * real arm64 -> x86_64 XPC IPC; no D3DMetal, VM, or game is required.
 */
#include "neptune/npt_work_context.h"
#include <mach/mach.h>
#include <mach/mach_voucher.h>
#include <mach/task_policy.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern char **environ;

static void transfer(int fd, void *data, size_t size, bool sending)
{
   char *p = static_cast<char *>(data);
   while (size) {
      ssize_t n = sending ? write(fd, p, size) : read(fd, p, size);
      assert(n > 0);
      p += n;
      size -= n;
   }
}

static void bank(char *text, size_t capacity)
{
   text[0] = 0;
   mach_port_t voucher = MACH_PORT_NULL;
   if (thread_get_mach_voucher(pthread_mach_thread_np(pthread_self()), 0, &voucher) == KERN_SUCCESS && voucher) {
      mach_msg_type_number_t size = static_cast<mach_msg_type_number_t>(capacity - 1);
      mach_voucher_extract_attr_content(voucher, MACH_VOUCHER_ATTR_KEY_BANK,
                                       reinterpret_cast<mach_voucher_attr_content_t>(text), &size);
      mach_port_deallocate(mach_task_self(), voucher);
   }
}

struct check_call {
   void *worker;
   pid_t owner;
   bool expected, nested;
   std::atomic<unsigned> *count;
};

static void verify(void *argument)
{
   auto *call = static_cast<check_call *>(argument);
   char value[512] = {}, expected[128];
   bank(value, sizeof(value));
   snprintf(expected, sizeof(expected), "holder pid %d with merchant pid %d", call->owner, getpid());
   assert((strstr(value, expected) != nullptr) == call->expected);
   ++*call->count;
   if (call->nested) {
      check_call inner = *call;
      inner.nested = false;
      npt_work_context_execute(call->worker, 2, 0, nullptr, 1, verify, &inner);
      char after[512] = {};
      bank(after, sizeof(after));
      assert(!strcmp(value, after));
   }
}

static void throws(void *)
{
   throw std::runtime_error("native call failure");
}

static void run_calls(void *worker, pid_t owner, bool expected)
{
   std::atomic<unsigned> count{0};
   std::vector<std::thread> threads;
   for (unsigned t = 0; t < 4; ++t) {
      threads.emplace_back([&] {
         for (unsigned i = 0; i < 128; ++i) {
            char before[512] = {}, after[512] = {};
            bank(before, sizeof(before));
            check_call call{worker, owner, expected, i % 32 == 0, &count};
            npt_work_context_execute(worker, 1, i, nullptr, 1, verify, &call);
            bank(after, sizeof(after));
            assert(!strcmp(before, after));
         }
         char before[512] = {}, after[512] = {};
         bank(before, sizeof(before));
         bool caught = false;
         try { npt_work_context_execute(worker, 1, 0, nullptr, 1, throws, nullptr); }
         catch (const std::runtime_error &) { caught = true; }
         assert(caught);
         bank(after, sizeof(after));
         assert(!strcmp(before, after));
         check_call call{worker, owner, expected, false, &count};
         npt_work_context_execute(worker, 1, 0, nullptr, 1, verify, &call);
      });
   }
   for (auto &thread : threads) thread.join();
   assert(count == 532); // 512 direct + 16 nested + 4 after exception.
}

static task_role_t task_role(void)
{
   task_category_policy_data_t policy{};
   mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
   boolean_t get_default = FALSE;
   assert(task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                          (task_policy_t)&policy, &count, &get_default) == KERN_SUCCESS);
   assert(count == TASK_CATEGORY_POLICY_COUNT && !get_default);
   return policy.role;
}

static void verify_role_once(void *worker)
{
   assert(task_role() == TASK_FOREGROUND_APPLICATION);
   /* Simulate a later policy owner changing this isolated test worker. A
    * repeated device notification must not fight it by applying role 1 again. */
   task_category_policy_data_t policy = { TASK_BACKGROUND_APPLICATION };
   assert(task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                          (task_policy_t)&policy, TASK_CATEGORY_POLICY_COUNT) == KERN_SUCCESS);
   assert(task_role() == TASK_BACKGROUND_APPLICATION);
   npt_work_context_device_created(worker);
   npt_work_context_device_created(worker);
   assert(task_role() == TASK_BACKGROUND_APPLICATION);
}

static int worker_main(int fd, bool supported)
{
   const task_role_t initial_role = task_role();
   assert(initial_role != TASK_FOREGROUND_APPLICATION);
   npt_work_context_config config;
   transfer(fd, &config, sizeof(config), false);
   void *worker = npt_work_context_worker_create(1, &config, supported);
   char command;
   while (read(fd, &command, 1) == 1) {
      if (command == 'q') break;
      if (command == 'd') {
         npt_work_context_device_created(worker);
         assert(task_role() == initial_role); // No authenticated, supported peer.
      } else if (command == 'a') {
         assert(task_role() == initial_role); // Ready alone must not apply policy.
         npt_work_context_device_created(worker);
         verify_role_once(worker);
      } else if (command == 'v') {
         verify_role_once(worker); // The earlier device event was held until ready.
      } else {
         if (command == 'f') usleep(200000); // Allow asynchronous peer cancellation.
         run_calls(worker, static_cast<pid_t>(config.owner_pid), command == 'r');
      }
      transfer(fd, &command, 1, true);
   }
   npt_work_context_worker_destroy(worker);
   return 0;
}

struct session {
   void *client;
   pid_t pid;
   int fd;
};

static session start(const char *executable, const char *mode)
{
   int sockets[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
   posix_spawn_file_actions_t actions;
   assert(posix_spawn_file_actions_init(&actions) == 0);
   assert(posix_spawn_file_actions_addclose(&actions, sockets[0]) == 0);
   char fd[32];
   snprintf(fd, sizeof(fd), "%d", sockets[1]);
   char *args[] = {const_cast<char *>(executable), const_cast<char *>("--worker"), fd,
                   const_cast<char *>(!strcmp(mode, "unsupported") ? "0" : "1"), nullptr};
   session result{};
   assert(posix_spawn(&result.pid, executable, &actions, nullptr, args, environ) == 0);
   posix_spawn_file_actions_destroy(&actions);
   close(sockets[1]);
   result.fd = sockets[0];
   npt_work_context_config config;
   uint32_t expected = static_cast<uint32_t>(result.pid);
   if (!strcmp(mode, "wrong-pid")) expected = static_cast<uint32_t>(getpid());
   result.client = npt_work_context_client_create(1, expected, &config);
   if (!strcmp(mode, "wrong-nonce")) config.nonce[0] ^= 1;
   transfer(result.fd, &config, sizeof(config), true);
   return result;
}

static void wait_status(int expected)
{
   for (unsigned i = 0; i < 650 && npt_work_context_status() != expected; ++i) usleep(10000);
   if (npt_work_context_status() != expected) {
      fprintf(stderr, "expected status %d got %d\n", expected, npt_work_context_status());
      abort();
   }
}

static void command(session &s, char value)
{
   transfer(s.fd, &value, 1, true);
   char reply;
   transfer(s.fd, &reply, 1, false);
   assert(value == reply);
}

static void stop(session &s)
{
   npt_work_context_client_destroy(s.client);
   s.client = nullptr;
   char quit = 'q';
   transfer(s.fd, &quit, 1, true);
   close(s.fd);
   int status;
   assert(waitpid(s.pid, &status, 0) == s.pid);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int startup_races(const char *service)
{
   setenv("NPT_WORK_CONTEXT_SERVICE", service, 1);
   for (unsigned i = 0; i < 64; ++i) {
      npt_work_context_config config{};
      config.owner_pid = static_cast<uint32_t>(getppid());
      arc4random_buf(config.nonce, sizeof(config.nonce));
      void *worker = npt_work_context_worker_create(i + 1, &config, true);
      assert(worker);
      // Exercise both immediate close and broker failure racing startup.
      if (i % 2) usleep(1000);
      npt_work_context_worker_destroy(worker);
   }
   // Let deferred setup timeouts run after all owners have been destroyed.
   usleep(5300000);
   assert(npt_work_context_status() == 0);
   fprintf(stderr, "PASS startup-races: 64 unavailable/immediate-close lifetimes\n");
   return 0;
}

int main(int argc, char **argv)
{
   if (argc == 3 && !strcmp(argv[1], "--startup-races"))
      return startup_races(argv[2]);
   if (argc == 3 && !strcmp(argv[1], "--service"))
      return npt_work_context_service_main(argv[2]);
   if (argc == 4 && !strcmp(argv[1], "--worker"))
      return worker_main(atoi(argv[2]), atoi(argv[3]) != 0);
   assert(argc == 4);
   setenv("NPT_WORK_CONTEXT_SERVICE", argv[1], 1);
   const char *mode = argv[3];
   session first = start(argv[2], mode);
   bool device_first = !strcmp(mode, "device-first");
   if (device_first) {
      // The Python harness has stopped only this test's broker until this ack.
      command(first, 'd');
      printf("DEVICE_READY\n");
      fflush(stdout);
   }
   bool success = !strcmp(mode, "success") || !strcmp(mode, "aggregate") || device_first;
   wait_status(success ? 1 : -1);
   command(first, success ? (device_first ? 'v' : 'a') : 'd');
   command(first, success ? 'r' : 'f');
   if (!strcmp(mode, "success")) {
      usleep(5300000); // Outlive broker setup timeout and the old token expiry.
      wait_status(1);
      command(first, 'r');
   }
   if (!strcmp(mode, "aggregate")) {
      session second = start(argv[2], "unsupported");
      wait_status(-1);
      stop(second);
      wait_status(1);
      command(first, 'r');
   }
   npt_work_context_client_destroy(first.client);
   first.client = nullptr;
   wait_status(0);
   command(first, 'f');
   stop(first);
   wait_status(0);
   fprintf(stderr, "PASS %s: device/ready role gate, one policy attempt, scoped BANK, concurrent/nested calls, exception restore, close/status\n", mode);
   return 0;
}
