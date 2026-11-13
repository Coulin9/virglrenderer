/* SPDX-License-Identifier: MIT
 * One unfinished XPC operation represents the lifetime of a VM-owned render
 * context. Its IPC attributes are installed only around finite rendering calls,
 * including work read directly from the shared ring without a VM doorbell.
 * The per-user broker exchanges endpoints once; rendering never passes through it.
 */
#import <Foundation/Foundation.h>
#include <Block.h>
#include <exception>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_voucher.h>
#include <mach/task_policy.h>
#include <pthread.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include "npt_work_context.h"

#define SETUP_TIMEOUT_NS (5ull * NSEC_PER_SEC)
#define MAX_BROKER_PEERS 128

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned status_pending, status_active, status_failed, status_generation;

int npt_work_context_status(void)
{
   pthread_mutex_lock(&status_mutex);
   int result = status_failed ? -1 : status_pending || !status_active ? 0 : 1;
   pthread_mutex_unlock(&status_mutex);
   return result;
}

void npt_work_context_status_reset(void)
{
   pthread_mutex_lock(&status_mutex);
   ++status_generation;
   status_pending = status_active = status_failed = 0;
   pthread_mutex_unlock(&status_mutex);
}

static unsigned *status_counter(int state)
{
   return state < 0 ? &status_failed : state > 0 ? &status_active : &status_pending;
}

static void snapshot(const char *stage, uint32_t ctx, uint64_t sequence)
{
   mach_port_t voucher = MACH_PORT_NULL;
   kern_return_t kr = thread_get_mach_voucher(pthread_mach_thread_np(pthread_self()), 0, &voucher);
   char bank[512] = {0};
   mach_msg_type_number_t size = sizeof(bank) - 1;
   kern_return_t bank_kr = KERN_INVALID_ARGUMENT;
   if (kr == KERN_SUCCESS && voucher != MACH_PORT_NULL) {
      bank_kr = mach_voucher_extract_attr_content(voucher, MACH_VOUCHER_ATTR_KEY_BANK,
                                                  (mach_voucher_attr_content_t)bank, &size);
      mach_port_deallocate(mach_task_self(), voucher);
   }
   for (char *p = bank; *p; ++p)
      if (*p == '\n' || *p == '\r') *p = ' ';
   uint64_t tid = 0;
   pthread_threadid_np(pthread_self(), &tid);
   fprintf(stderr, "AppleDeck work-context stage=%s pid=%d tid=%llu ctx=%u sequence=%llu voucher_get=%d bank_get=%d bank=\"%s\"\n",
           stage, getpid(), (unsigned long long)tid, ctx,
           (unsigned long long)sequence, kr, bank_kr, bank);
}

struct call_frame {
   void (*function)(void *);
   void *argument;
   uint32_t context_id;
   uint64_t sequence;
   bool trace;
   std::exception_ptr exception;
   struct call_frame *previous;
};
static thread_local struct call_frame *current_call;

static void invoke_current_call(void)
{
   struct call_frame *frame = current_call;
   if (frame->trace) snapshot("execute-inside", frame->context_id, frame->sequence);
   /* libdispatch blocks must return normally even if the native call throws. */
   try { frame->function(frame->argument); }
   catch (...) { frame->exception = std::current_exception(); }
}

@interface NptWorkContext : NSObject {
@public
   uint32_t context_id, worker_pid;
   struct npt_work_context_config config;
   dispatch_queue_t queue;
   xpc_connection_t broker, listener, peer;
   xpc_object_t unfinished_reply;
   pthread_mutex_t mutex;
   dispatch_block_t carrier;
   uint64_t sequence;
   bool closing, tracked, trace, device_ready, role_attempted;
   unsigned generation;
   int state;
}
- (void)setState:(int)value;
- (void)fail:(const char *)reason;
- (void)closeConnections;
- (void)applyDeviceRole;
@end

static void cancel_connection(xpc_connection_t *connection)
{
   if (!*connection) return;
   xpc_connection_cancel(*connection);
   xpc_release(*connection);
   *connection = NULL;
}

@implementation NptWorkContext
- (id)init
{
   self = [super init];
   if (self) {
      pthread_mutex_init(&mutex, NULL);
      queue = dispatch_queue_create("org.appledeck.render-context", DISPATCH_QUEUE_SERIAL);
      trace = getenv("APPLEDECK_WORK_CONTEXT_TRACE") && strcmp(getenv("APPLEDECK_WORK_CONTEXT_TRACE"), "0");
   }
   return self;
}
- (void)setState:(int)value
{
   pthread_mutex_lock(&status_mutex);
   if (tracked && generation == status_generation) {
      --*status_counter(state);
      ++*status_counter(value);
   }
   state = value;
   pthread_mutex_unlock(&status_mutex);
}
- (void)applyDeviceRole
{
   /* Native device initialization can change the task's role. Wait for both
    * that initialization and authenticated VM ownership, in either order.
    * This queue owns the latch; later policy changes never cause a retry. */
   if (closing || state != 1 || !device_ready || role_attempted) return;
   role_attempted = true;
   task_category_policy_data_t policy = { TASK_FOREGROUND_APPLICATION };
   kern_return_t kr = task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                                      (task_policy_t)&policy, TASK_CATEGORY_POLICY_COUNT);
   task_category_policy_data_t actual = { TASK_UNSPECIFIED };
   mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
   boolean_t get_default = FALSE;
   kern_return_t read_kr = task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                                          (task_policy_t)&actual, &count, &get_default);
   fprintf(stderr, "AppleDeck work-context device-role: pid=%d ctx=%u set=%d get=%d role=%d count=%u default=%d\n",
           getpid(), context_id, kr, read_kr, actual.role, count, get_default);
   /* A policy failure must not disable IPC attribution or rendering. */
}
- (void)closeConnections
{
   /* This method and every connection callback run on our serial queue. */
   pthread_mutex_lock(&mutex);
   dispatch_block_t old = carrier;
   carrier = NULL;
   pthread_mutex_unlock(&mutex);
   if (old) Block_release(old);
   if (unfinished_reply) {
      xpc_dictionary_set_bool(unfinished_reply, "closed", true);
      if (peer) xpc_connection_send_message(peer, unfinished_reply);
      xpc_release(unfinished_reply);
      unfinished_reply = NULL;
   }
   cancel_connection(&broker);
   cancel_connection(&peer);
   cancel_connection(&listener);
}
- (void)fail:(const char *)reason
{
   if (closing || state < 0) return;
   fprintf(stderr, "AppleDeck work-context unavailable: pid=%d ctx=%u %s\n", getpid(), context_id, reason);
   [self setState:-1];
   [self closeConnections];
}
- (void)dealloc
{
   pthread_mutex_destroy(&mutex);
   dispatch_release(queue);
   [super dealloc];
}
@end

static bool matching_session(xpc_object_t message, NptWorkContext *context)
{
   size_t size = 0;
   const void *nonce = xpc_dictionary_get_data(message, "nonce", &size);
   return nonce && size == sizeof(context->config.nonce) &&
          !memcmp(nonce, context->config.nonce, size) &&
          xpc_dictionary_get_uint64(message, "ctx") == context->context_id &&
          xpc_dictionary_get_uint64(message, "owner") == context->config.owner_pid;
}

static xpc_object_t session_message(NptWorkContext *context, const char *operation)
{
   xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
   xpc_dictionary_set_string(message, "op", operation);
   xpc_dictionary_set_uint64(message, "ctx", context->context_id);
   xpc_dictionary_set_uint64(message, "owner", context->config.owner_pid);
   xpc_dictionary_set_uint64(message, "worker", context->worker_pid);
   xpc_dictionary_set_data(message, "nonce", context->config.nonce, sizeof(context->config.nonce));
   return message;
}

static void setup_timeout(NptWorkContext *context)
{
   dispatch_after(dispatch_time(DISPATCH_TIME_NOW, SETUP_TIMEOUT_NS), context->queue, ^{
      if (!context->closing && context->state == 0)
         [context fail:"setup timed out"];
   });
}

static void client_connect(NptWorkContext *client, xpc_endpoint_t endpoint)
{
   client->peer = xpc_connection_create_from_endpoint(endpoint);
   xpc_connection_set_target_queue(client->peer, client->queue);
   xpc_connection_set_event_handler(client->peer, ^(xpc_object_t event) {
      if (client->closing || client->state < 0) return;
      if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) {
         [client fail:"worker connection closed"];
         return;
      }
      const char *operation = xpc_dictionary_get_string(event, "op");
      if (!operation || strcmp(operation, "ready") || !matching_session(event, client) ||
          xpc_connection_get_euid(client->peer) != geteuid() ||
          xpc_connection_get_pid(client->peer) != (pid_t)client->worker_pid ||
          !xpc_dictionary_get_bool(event, "supported")) {
         [client fail:"worker readiness/identity rejected"];
         return;
      }
      [client setState:1];
      fprintf(stderr, "AppleDeck work-context ready: vm=%u worker=%u ctx=%u\n",
              client->config.owner_pid, client->worker_pid, client->context_id);
   });
   xpc_connection_resume(client->peer);
   xpc_object_t start = session_message(client, "render-context");
   /* This reply deliberately remains outstanding until the context closes. It
    * represents ongoing rendering for this VM, not an already completed batch. */
   xpc_connection_send_message_with_reply(client->peer, start, client->queue, ^(xpc_object_t reply) {
      (void)reply;
      if (!client->closing) [client fail:"render-context operation ended"];
   });
   xpc_release(start);
}

void *npt_work_context_client_create(uint32_t context_id, uint32_t worker_pid,
                                    struct npt_work_context_config *config)
{
   *config = {};
   const char *service = getenv("NPT_WORK_CONTEXT_SERVICE");
   if (!service || !*service) return NULL;
   NptWorkContext *client = [[NptWorkContext alloc] init];
   client->context_id = context_id;
   client->worker_pid = worker_pid;
   client->config.owner_pid = (uint32_t)getpid();
   arc4random_buf(client->config.nonce, sizeof(client->config.nonce));
   *config = client->config;
   pthread_mutex_lock(&status_mutex);
   client->tracked = true;
   client->generation = status_generation;
   ++status_pending;
   pthread_mutex_unlock(&status_mutex);
   /* Start asynchronously: unavailable attribution must not prevent rendering. */
   char *service_copy = strdup(service);
   dispatch_async(client->queue, ^{
      if (client->closing) { free(service_copy); return; }
      if (!worker_pid || !service_copy) {
         free(service_copy);
         [client fail:"worker identity unavailable"];
         return;
      }
      client->broker = xpc_connection_create_mach_service(service_copy, client->queue, 0);
      free(service_copy);
      xpc_connection_set_event_handler(client->broker, ^(xpc_object_t event) {
         (void)event;
         if (client->broker && !client->closing) [client fail:"rendezvous unavailable"];
      });
      xpc_connection_resume(client->broker);
      xpc_object_t request = session_message(client, "wait");
      xpc_connection_send_message_with_reply(client->broker, request, client->queue, ^(xpc_object_t reply) {
         if (client->closing || client->state < 0) return;
         if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY ||
             !xpc_dictionary_get_bool(reply, "ok")) {
            [client fail:"rendezvous rejected session"];
            return;
         }
         if (!xpc_dictionary_get_bool(reply, "supported")) {
            [client fail:"native backend does not support work-context propagation"];
            return;
         }
         xpc_object_t endpoint = xpc_dictionary_get_value(reply, "endpoint");
         if (!endpoint || xpc_get_type(endpoint) != XPC_TYPE_ENDPOINT) {
            [client fail:"rendezvous returned no worker endpoint"];
            return;
         }
         client_connect(client, endpoint);
         cancel_connection(&client->broker);
      });
      xpc_release(request);
      setup_timeout(client);
   });
   return client;
}

static void context_destroy(NptWorkContext *context)
{
   if (!context) return;
   dispatch_sync(context->queue, ^{
      context->closing = true;
      [context closeConnections];
      pthread_mutex_lock(&status_mutex);
      if (context->tracked && context->generation == status_generation)
         --*status_counter(context->state);
      context->tracked = false;
      pthread_mutex_unlock(&status_mutex);
   });
   [context release];
}

void npt_work_context_client_destroy(void *client)
{
   context_destroy((NptWorkContext *)client);
}

void *npt_work_context_worker_create(uint32_t context_id,
                                    const struct npt_work_context_config *config,
                                    bool supported)
{
   const char *service = getenv("NPT_WORK_CONTEXT_SERVICE");
   if (!config->owner_pid || !service || !*service) return NULL;
   NptWorkContext *worker = [[NptWorkContext alloc] init];
   worker->context_id = context_id;
   worker->worker_pid = (uint32_t)getpid();
   worker->config = *config;
   char *service_copy = strdup(service);
   /* Setup, error handling and teardown share one queue: an immediately failed
    * broker cannot release listener/broker while setup is still using them. */
   dispatch_async(worker->queue, ^{
      if (worker->closing) { free(service_copy); return; }
      if (!service_copy) {
         [worker fail:"could not allocate service name"];
         return;
      }
      worker->listener = xpc_connection_create(NULL, worker->queue);
      xpc_connection_set_event_handler(worker->listener, ^(xpc_object_t connection) {
         if (xpc_get_type(connection) != XPC_TYPE_CONNECTION) {
            if (!worker->closing && worker->listener) [worker fail:"listener closed"];
            return;
         }
         if (worker->closing || worker->state < 0 || worker->peer ||
             xpc_connection_get_euid(connection) != geteuid() ||
             xpc_connection_get_pid(connection) != (pid_t)worker->config.owner_pid) {
            xpc_connection_cancel(connection);
            return;
         }
         worker->peer = xpc_retain(connection);
         xpc_connection_set_target_queue(connection, worker->queue);
         xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            if (worker->closing || worker->state < 0) return;
            if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
               [worker fail:"VM connection closed"];
               return;
            }
            const char *operation = xpc_dictionary_get_string(message, "op");
            if (!operation || strcmp(operation, "render-context") ||
                !matching_session(message, worker) || worker->unfinished_reply || !supported) {
               [worker fail:"render-context request rejected"];
               return;
            }
            worker->unfinished_reply = xpc_dictionary_create_reply(message);
            if (!worker->unfinished_reply) {
               [worker fail:"render-context request has no reply port"];
               return;
            }
            /* Only IPC attributes are carried; each rendering thread retains its
             * existing QoS. No mutable arguments are captured in the reusable block. */
            dispatch_block_t captured = dispatch_block_create(
               (dispatch_block_flags_t)(DISPATCH_BLOCK_ASSIGN_CURRENT | DISPATCH_BLOCK_NO_QOS_CLASS), ^{
                  invoke_current_call();
               });
            if (!captured) {
               [worker fail:"could not capture VM IPC context"];
               return;
            }
            pthread_mutex_lock(&worker->mutex);
            worker->carrier = captured;
            pthread_mutex_unlock(&worker->mutex);
            [worker setState:1];
            [worker applyDeviceRole];
            xpc_object_t ready = session_message(worker, "ready");
            xpc_dictionary_set_bool(ready, "supported", true);
            xpc_connection_send_message(worker->peer, ready);
            xpc_release(ready);
         });
         xpc_connection_resume(connection);
      });
      xpc_connection_resume(worker->listener);
      worker->broker = xpc_connection_create_mach_service(service_copy, worker->queue, 0);
      free(service_copy);
      xpc_connection_set_event_handler(worker->broker, ^(xpc_object_t event) {
         (void)event;
         if (worker->broker && !worker->closing) [worker fail:"rendezvous unavailable"];
      });
      xpc_connection_resume(worker->broker);
      xpc_object_t request = session_message(worker, "publish");
      xpc_endpoint_t endpoint = xpc_endpoint_create(worker->listener);
      xpc_dictionary_set_value(request, "endpoint", endpoint);
      xpc_dictionary_set_bool(request, "supported", supported);
      xpc_release(endpoint);
      xpc_connection_send_message_with_reply(worker->broker, request, worker->queue, ^(xpc_object_t reply) {
         if (worker->closing || worker->state < 0) return;
         if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY || !xpc_dictionary_get_bool(reply, "ok"))
            [worker fail:"rendezvous rejected worker"];
         else {
            cancel_connection(&worker->broker);
            if (!supported) [worker fail:"native backend does not support work-context propagation"];
         }
      });
      xpc_release(request);
      setup_timeout(worker);
   });
   return worker;
}

void npt_work_context_worker_destroy(void *worker)
{
   /* The caller stops and joins ring threads before releasing this owner. */
   context_destroy((NptWorkContext *)worker);
}

void npt_work_context_device_created(void *opaque)
{
   NptWorkContext *worker = (NptWorkContext *)opaque;
   if (!worker) return;
   /* Called from a rendering thread, never this queue. The context owner joins
    * rendering threads before destroy. If already authenticated, finish the
    * one policy request before returning the newly created device to the guest. */
   dispatch_sync(worker->queue, ^{
      worker->device_ready = true;
      [worker applyDeviceRole];
   });
}

void npt_work_context_execute(void *opaque, uint64_t ring_id, uint32_t head,
                              void *queue, uint32_t count,
                              void (*function)(void *), void *argument)
{
   NptWorkContext *worker = (NptWorkContext *)opaque;
   if (!worker) { function(argument); return; }
   pthread_mutex_lock(&worker->mutex);
   dispatch_block_t captured = worker->carrier ? Block_copy(worker->carrier) : (dispatch_block_t)nil;
   uint64_t sequence = ++worker->sequence;
   pthread_mutex_unlock(&worker->mutex);
   if (!captured) { function(argument); return; }
   struct call_frame frame = {
      .function = function, .argument = argument, .context_id = worker->context_id,
      .sequence = sequence, .trace = worker->trace && sequence <= 128,
      .previous = current_call,
   };
   if (frame.trace) {
      fprintf(stderr, "AppleDeck work-context stage=execute-submit pid=%d ctx=%u sequence=%llu ring=%llu head=%u queue=%p count=%u\n",
              getpid(), worker->context_id, (unsigned long long)sequence,
              (unsigned long long)ring_id, head, queue, count);
      snapshot("execute-before", frame.context_id, sequence);
   }
   current_call = &frame;
   captured();
   current_call = frame.previous;
   if (frame.trace) snapshot("execute-after", frame.context_id, sequence);
   Block_release(captured);
   if (frame.exception) std::rethrow_exception(frame.exception);
}

/* The broker keeps only bounded setup requests. A random capability from the
 * private INIT socket must match both actual XPC peers before an endpoint leaves
 * the worker. A paired entry is removed immediately, and a disconnected/timed-out
 * peer removes its pending entry. The broker holds no rendering operation. */
@interface NptBrokerPeer : NSObject {
@public
   xpc_connection_t connection;
   xpc_object_t reply, endpoint;
   NSData *nonce;
   uint32_t context_id, owner_pid, worker_pid;
   bool publisher, supported, closed;
}
@end
@implementation NptBrokerPeer
- (void)dealloc
{
   if (reply) xpc_release(reply);
   if (endpoint) xpc_release(endpoint);
   [nonce release];
   [super dealloc];
}
@end

static void broker_remove(NptBrokerPeer *peer, NSMutableDictionary *pending,
                           NSMutableSet *peers)
{
   if (peer->closed) return;
   peer->closed = true;
   if (peer->nonce && [pending objectForKey:peer->nonce] == peer)
      [pending removeObjectForKey:peer->nonce];
   cancel_connection(&peer->connection);
   [peers removeObject:peer];
}

static bool broker_parse(NptBrokerPeer *peer, xpc_object_t message)
{
   const char *operation = xpc_dictionary_get_string(message, "op");
   if (!operation || (strcmp(operation, "wait") && strcmp(operation, "publish"))) return false;
   size_t size = 0;
   const void *nonce = xpc_dictionary_get_data(message, "nonce", &size);
   uint64_t ctx = xpc_dictionary_get_uint64(message, "ctx");
   uint64_t owner = xpc_dictionary_get_uint64(message, "owner");
   uint64_t worker = xpc_dictionary_get_uint64(message, "worker");
   if (!nonce || size != 16 || !ctx || ctx > UINT32_MAX || !owner || owner > INT32_MAX ||
       !worker || worker > INT32_MAX || owner == worker) return false;
   peer->publisher = !strcmp(operation, "publish");
   if (xpc_connection_get_pid(peer->connection) != (pid_t)(peer->publisher ? worker : owner)) return false;
   peer->nonce = [[NSData alloc] initWithBytes:nonce length:size];
   peer->context_id = (uint32_t)ctx;
   peer->owner_pid = (uint32_t)owner;
   peer->worker_pid = (uint32_t)worker;
   peer->reply = xpc_dictionary_create_reply(message);
   if (!peer->reply) return false;
   if (peer->publisher) {
      xpc_object_t endpoint = xpc_dictionary_get_value(message, "endpoint");
      if (!endpoint || xpc_get_type(endpoint) != XPC_TYPE_ENDPOINT) return false;
      peer->endpoint = xpc_retain(endpoint);
      peer->supported = xpc_dictionary_get_bool(message, "supported");
   }
   return true;
}

int npt_work_context_service_main(const char *service)
{
   dispatch_queue_t queue = dispatch_get_main_queue();
   NSMutableDictionary *pending = [[NSMutableDictionary alloc] init];
   NSMutableSet *peers = [[NSMutableSet alloc] init];
   xpc_connection_t listener = xpc_connection_create_mach_service(service, queue,
      XPC_CONNECTION_MACH_SERVICE_LISTENER);
   xpc_connection_set_event_handler(listener, ^(xpc_object_t connection) {
      @autoreleasepool {
         if (xpc_get_type(connection) != XPC_TYPE_CONNECTION) {
            fprintf(stderr, "AppleDeck work-context broker: Mach service unavailable\n");
            exit(EXIT_FAILURE);
         }
         if (xpc_connection_get_euid(connection) != geteuid() || [peers count] >= MAX_BROKER_PEERS) {
            xpc_connection_cancel(connection);
            return;
         }
         NptBrokerPeer *peer = [[NptBrokerPeer alloc] init];
         peer->connection = xpc_retain(connection);
         [peers addObject:peer];
         xpc_connection_set_target_queue(connection, queue);
         xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            @autoreleasepool {
               if (peer->closed) return;
               if (xpc_get_type(message) != XPC_TYPE_DICTIONARY || peer->nonce || !broker_parse(peer, message)) {
                  broker_remove(peer, pending, peers);
                  return;
               }
               NptBrokerPeer *other = [pending objectForKey:peer->nonce];
               if (!other) {
                  [pending setObject:peer forKey:peer->nonce];
                  return;
               }
               if (peer->publisher == other->publisher || peer->context_id != other->context_id ||
                   peer->owner_pid != other->owner_pid || peer->worker_pid != other->worker_pid) {
                  broker_remove(peer, pending, peers);
                  return;
               }
               NptBrokerPeer *worker = peer->publisher ? peer : other;
               NptBrokerPeer *client = peer->publisher ? other : peer;
               xpc_dictionary_set_bool(client->reply, "ok", true);
               xpc_dictionary_set_bool(client->reply, "supported", worker->supported);
               xpc_dictionary_set_value(client->reply, "endpoint", worker->endpoint);
               xpc_dictionary_set_bool(worker->reply, "ok", true);
               xpc_connection_send_message(client->connection, client->reply);
               xpc_connection_send_message(worker->connection, worker->reply);
               [pending removeObjectForKey:peer->nonce];
               if (getenv("APPLEDECK_WORK_CONTEXT_TRACE"))
                  fprintf(stderr, "AppleDeck work-context broker paired: vm=%u worker=%u ctx=%u\n",
                          peer->owner_pid, peer->worker_pid, peer->context_id);
            }
         });
         xpc_connection_resume(connection);
         dispatch_after(dispatch_time(DISPATCH_TIME_NOW, SETUP_TIMEOUT_NS), queue, ^{
            broker_remove(peer, pending, peers);
         });
         [peer release];
      }
   });
   xpc_connection_resume(listener);
   dispatch_main();
}
