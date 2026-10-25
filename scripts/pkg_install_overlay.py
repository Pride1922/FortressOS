"""Enable the normal executor in an isolated disposable test source copy."""
def overlay(work,replace):
    replace('src/fs/pkg_engine_config.h','#define FORTRESS_PKG_ENGINE 0','#define FORTRESS_PKG_ENGINE 1')
