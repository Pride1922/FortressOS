# Standalone private gate; does not include the release Makefile.
.PHONY: test-pkg-grant-candidate
test-pkg-grant-candidate:
	python3 scripts/test_pkg_grant_candidate.py

.PHONY: test-pkg-grant-fs
test-pkg-grant-fs:
	python3 scripts/test_pkg_grant_fs.py

.PHONY: test-pkg-grant-fs-negative test-pkg-grant-fs-recovery
test-pkg-grant-fs-negative:
	python3 scripts/test_pkg_grant_fs_negative.py

test-pkg-grant-fs-recovery:
	python3 scripts/test_pkg_grant_fs_recovery.py

.PHONY: test-pkg-grant-lifecycle
test-pkg-grant-lifecycle:
	python3 scripts/test_pkg_grant_lifecycle.py
