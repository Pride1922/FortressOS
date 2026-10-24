"""Private trust contract build and actual installed-root login/reboot campaign."""
import inspect,sys
import test_pkg_running_root as root
import pkg_trust_records as records
from pkg_trust_overlay import overlay
def main():
    root.overlay=overlay;root.records.build=records.build
    source=inspect.getsource(root.runner.main).replace('boot_mode=pkg-test-v1','boot_mode=pkg-trust-v2')
    exec(compile(source,'<trust boot builder>','exec'),vars(root.runner))
    root.main()
if __name__=='__main__':main()
