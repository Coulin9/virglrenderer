/* VM-owned rendering sessions. Shared-ring command data stays on the ring. */
#ifndef NPT_WORK_CONTEXT_H
#define NPT_WORK_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

/* Sent only over the private VM/worker initialization socket. */
struct npt_work_context_config {
   uint8_t nonce[16];
   uint32_t owner_pid;
};

/* Optional native backend API. The setter copies a synchronous submission
 * runner for its worker process, or releases it when passed NULL. */
#ifdef __APPLE__
typedef void (^npt_work_context_metal_runner)(const char *stage, void (^submit)(void));
#else
typedef void *npt_work_context_metal_runner;
#endif
typedef void (*npt_work_context_set_metal_runner)(npt_work_context_metal_runner runner);

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__APPLE__) && defined(ENABLE_NEPTUNE)
void *npt_work_context_client_create(uint32_t context_id, uint32_t worker_pid,
                                    struct npt_work_context_config *config);
void npt_work_context_client_destroy(void *client);
int npt_work_context_status(void);
void npt_work_context_status_reset(void);
void npt_work_context_set_active(bool active);
int npt_work_context_probe_service(const char *service, uint32_t timeout_ms);
void *npt_work_context_worker_create(uint32_t context_id,
                                    const struct npt_work_context_config *config,
                                    bool supported,
                                    npt_work_context_set_metal_runner set_metal_runner);
void npt_work_context_worker_destroy(void *worker);
/* Call only for a standalone worker after a native device was created. */
void npt_work_context_device_created(void *worker);
void npt_work_context_execute(void *worker, uint64_t ring_id, uint32_t head,
                              void *queue, uint32_t count,
                              void (*function)(void *), void *argument);
int npt_work_context_service_main(const char *service);
#else
static inline void *npt_work_context_client_create(uint32_t id, uint32_t pid,
                                                   struct npt_work_context_config *cfg)
{ (void)id; (void)pid; *cfg = (struct npt_work_context_config){0}; return 0; }
static inline void npt_work_context_client_destroy(void *p) { (void)p; }
static inline int npt_work_context_status(void) { return 0; }
static inline void npt_work_context_status_reset(void) {}
static inline void npt_work_context_set_active(bool active) { (void)active; }
static inline int npt_work_context_probe_service(const char *service, uint32_t timeout_ms)
{ (void)service; (void)timeout_ms; return 0; }
static inline void *npt_work_context_worker_create(uint32_t id,
   const struct npt_work_context_config *cfg, bool supported,
   npt_work_context_set_metal_runner set_metal_runner)
{ (void)id; (void)cfg; (void)supported; (void)set_metal_runner; return 0; }
static inline void npt_work_context_worker_destroy(void *p) { (void)p; }
static inline void npt_work_context_device_created(void *p) { (void)p; }
static inline void npt_work_context_execute(void *p, uint64_t ring, uint32_t head,
   void *queue, uint32_t count, void (*fn)(void *), void *arg)
{ (void)p; (void)ring; (void)head; (void)queue; (void)count; fn(arg); }
#endif
#ifdef __cplusplus
}
#endif
#endif
