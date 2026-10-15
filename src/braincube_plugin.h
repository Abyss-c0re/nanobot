#ifndef NANOBOT_BRAINCUBE_PLUGIN_H
#define NANOBOT_BRAINCUBE_PLUGIN_H
#include <stddef.h>

/* Plugin around third_party/braincube (LHLAM). No-op stubs if not linked. */

void ng_bc_init(void);
/* Parent-only: start continuous on-robot learning thread (before HTTP fork). */
void ng_bc_boot_parent(void);
/* Free-form JSON status (malloc'd). */
char *ng_bc_status_json(void);
/* Real-time sensor-cubes + MetaCube activity for wrapper viz (malloc'd). */
char *ng_bc_live_json(void);
/* Handle POST JSON body; returns malloc'd JSON response. */
char *ng_bc_handle_post(const char *json_body);
/* Dual-wire train_status plate (malloc'd JSON · machine fields only). */
char *ng_bc_train_status_report(void);
/* Log one-liner after supervision then purge bulky logs (keep robot lean). */
void ng_bc_log_supervision_oneliner(const char *line);
/* Start/stop continuous learning / plane auto-adapt. */
void ng_bc_set_auto_adapt(int on);
void ng_bc_set_continuous(int on);
void ng_bc_set_direct_io(int on);
int ng_bc_auto_adapt(void);
int ng_bc_continuous(void);
int ng_bc_direct_io(void);
int ng_bc_available(void);

/* Plugin identity (for VCS / about). */
#define NG_BC_PLUGIN_VERSION "0.3.0-crimson-law"

#endif
