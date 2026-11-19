/* Self-contained checks for the AppleDeck per-context backend tag helpers:
 * both tagged backends must resolve, and untagged/legacy names (including
 * near-prefix lookalikes) must stay on the inherited path. */

#include <assert.h>
#include <string.h>
#include "../src/neptune/npt_library_names.h"

int
main(void)
{
   /* Tagged contexts resolve to their backend and allowlist variable. */
   assert(!strcmp(npt_appledeck_ctx_backend("ad:d:game.exe"), "d3dmetal"));
   assert(!strcmp(npt_appledeck_ctx_backend("ad:m:game.exe"), "mullion"));
   assert(!strcmp(npt_appledeck_ctx_backend("ad:d:"), "d3dmetal"));

   /* The adapter env mapping is a fixed allowlist, never guest-derived. */
   assert(!strcmp(npt_appledeck_adapter_env("d3dmetal"),
                  "NPT_APPLEDECK_D3DMETAL_ADAPTER"));
   assert(!strcmp(npt_appledeck_adapter_env("mullion"),
                  "NPT_APPLEDECK_MULLION_ADAPTER"));
   assert(npt_appledeck_adapter_env("dxmt") == NULL);
   assert(npt_appledeck_adapter_env("ad:d:x") == NULL);

   /* Legacy/untagged contexts keep the inherited path. */
   assert(npt_appledeck_ctx_backend(NULL) == NULL);
   assert(npt_appledeck_ctx_backend("") == NULL);
   assert(npt_appledeck_ctx_backend("game.exe") == NULL);
   assert(npt_appledeck_ctx_backend("ad:d") == NULL);      /* short prefix */
   assert(npt_appledeck_ctx_backend("ad:x:game.exe") == NULL);
   assert(npt_appledeck_ctx_backend("AD:D:game.exe") == NULL);
   assert(npt_appledeck_ctx_backend("ad:dm:game.exe") == NULL);
   return 0;
}
