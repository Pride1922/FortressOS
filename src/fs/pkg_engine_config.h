#ifndef FORTRESS_PKG_ENGINE_CONFIG_H
#define FORTRESS_PKG_ENGINE_CONFIG_H
/* Executor code is dormant in ordinary roots. Enrolled test adapters enable
 * compilation in an isolated copy; runtime authority still requires admission. */
#ifndef FORTRESS_PKG_ENGINE
#define FORTRESS_PKG_ENGINE 0
#endif
/* Trust codecs need kernel crypto linkage and projected mount admission before
 * normal enablement. Private compatibility units enable them explicitly. */
#ifndef FORTRESS_PKG_TRUST
#define FORTRESS_PKG_TRUST 0
#endif
#endif
