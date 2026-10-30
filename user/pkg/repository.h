#ifndef FORTRESS_PKG_REPOSITORY_H
#define FORTRESS_PKG_REPOSITORY_H
#include "catalog.h"
#define PKG_REPO_BASE_MAX 160u
#define PKG_REPO_URL_MAX 256u
typedef struct {char base[PKG_REPO_BASE_MAX];} pkg_repository;
/* Exact version=1\nurl=...\n, bounded ASCII, canonical URL without credentials,
 * query/fragment/percent escapes, dot segments or ambiguous host labels.
 * Caller memory is accessible; failure preserves disjoint output. */
int pkg_repository_decode(const uint8_t *,size_t,pkg_repository *);
int pkg_repository_url(const pkg_repository *,const uint8_t *artifact_hash,char [PKG_REPO_URL_MAX]);
typedef struct {fpk_catalog_policy catalog;uint8_t platform[32];uint64_t base_version;} pkg_repository_trust;
/* Root-owned repository UUID/public keys and local compatibility, no device
 * identity, boot selector or secret. Exact ordered LF-terminated fields. */
int pkg_repository_trust_decode(const uint8_t *,size_t,uint64_t,pkg_repository_trust *);
/* Creates a NEW caller-selected staging directory; never installs files. */
int pkg_fetch_main(int,char **);
#endif
