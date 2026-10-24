/* Private finite-profile configuration and workspace. Runtime fixture authority
 * is loaded separately from immutable boot-module files by the test harness.
 * This file and the extracted functions are never linked into release media. */
#define XI_CERT_BODY 1536u
static bool ad_enabled,ad_pair_staged;
static pe_enrollment ad_en;
static uint8_t ad_descriptor[PE_MAX],ad_state[1024],ad_slot[1024];
static const char *ad_paths[16]={
 "/mnt/pkg/enrollment.v1","/mnt/pkg/grant.v1","/mnt/pkg/ownership.v1","/mnt/pkg/transaction.v1",
 "/mnt/pkg/origin.v1","/mnt/pkg/account-generation.v1","/mnt/pkg/state.v1","/mnt/pkg/trust.v1",
 "/mnt/pkg","/mnt/etc","/mnt/var","/mnt/var/lib","/mnt/var/lib/fortress",
 "/mnt/var/lib/fortress/accounts","/mnt/usr/bin","/mnt/usr/share/pkg"
};
static fpk_catalog_policy ng_policy;
static bool ng_required;
static uint8_t ng_platform[32];static uint64_t ng_base;
static const char *xr_paths[]={"/mnt/pkg/retirement.v1","/mnt/pkg/retirement.cert","/mnt/pkg/enrollment.cert","/mnt/pkg/evidence.fpk"};
static uint8_t xr_locations[512],xi_locator[128],xi_pin[32];
static pe_policy xr_en_policy;static pa_policy xr_policy;
static unsigned xi_stage,xr_previews,xi_commits,xi_max;
static bool xr_bootstrap,xi_manual;
typedef struct {
 uint8_t m[512],g[1024],t[8192],l[1024],a[1024],o[1024],s[1024],tr[1024],intent[1024];
 uint8_t en[PE_MAX],cert[PA_MAX],art[8192],exe[8192];pa_input input;
} xr_bundle;
static xr_bundle xr_new,xi_current,xi_next;
static pa_workspace xr_workspace;static pa_result xr_result;
static const char *xi_cert_path="/mnt/pkg/completion.cert";
static uint8_t xi_cert[1728];
static const char *xi_names[]={"/mnt/pkg/transaction.v1/artifact.fpk","/mnt/usr/bin/demo","/mnt/pkg/transaction.v1/next.v1","/mnt/pkg/transaction.v1/retired.v1"};
static uint32_t sv_roles[ACCOUNT_ROLE_COUNT];static char sv_paths[ACCOUNT_ROLE_COUNT][96];
