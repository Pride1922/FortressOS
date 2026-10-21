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

.PHONY: test-pkg-grant-idle
test-pkg-grant-idle:
	python3 scripts/test_pkg_grant_idle.py

.PHONY: test-pkg-grant-compete
test-pkg-grant-compete:
	python3 scripts/test_pkg_grant_compete.py

.PHONY: test-pkg-account-capacity test-pkg-writer-lifetime
test-pkg-account-capacity:
	python3 scripts/test_pkg_account_capacity.py

test-pkg-writer-lifetime:
	python3 scripts/test_pkg_writer_lifetime.py

.PHONY: test-pkg-service-integration
test-pkg-service-integration:
	python3 scripts/test_pkg_service_integration.py
