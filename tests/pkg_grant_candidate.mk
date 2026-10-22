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

.PHONY: test-pkg-grant-prereplay
test-pkg-grant-prereplay:
	python3 scripts/test_pkg_grant_prereplay.py

.PHONY: test-pkg-service-prereplay
test-pkg-service-prereplay:
	python3 scripts/test_pkg_service_integration.py --pre-replay

.PHONY: test-pkg-mutation-admission test-pkg-mutation-context test-pkg-mutation-context-tsan
test-pkg-mutation-admission:
	python3 scripts/test_pkg_mutation_admission.py

test-pkg-mutation-context:
	python3 scripts/test_pkg_mutation_context.py

test-pkg-mutation-context-tsan:
	python3 scripts/test_pkg_mutation_context.py --tsan

.PHONY: test-pkg-call-binding test-pkg-call-namespace test-pkg-call-namespace-tsan
test-pkg-call-binding:
	python3 scripts/test_pkg_call_binding.py

test-pkg-call-namespace:
	python3 scripts/test_pkg_mutation_context.py --binding

test-pkg-call-namespace-tsan:
	python3 scripts/test_pkg_mutation_context.py --binding --tsan

.PHONY: test-pkg-cancel-recovery test-pkg-cancel-recovery-tsan test-pkg-call-kernel
test-pkg-cancel-recovery:
	python3 scripts/test_pkg_cancel_recovery.py

test-pkg-cancel-recovery-tsan:
	python3 scripts/test_pkg_cancel_recovery.py --block 4096 --tsan

test-pkg-call-kernel:
	python3 scripts/test_pkg_call_kernel.py

.PHONY: test-pkg-account-cancel test-pkg-account-cancel-tsan test-pkg-kernel-fs
test-pkg-account-cancel:
	python3 scripts/test_pkg_cancel_recovery.py --account

test-pkg-account-cancel-tsan:
	python3 scripts/test_pkg_cancel_recovery.py --account --block 4096 --tsan

test-pkg-kernel-fs:
	python3 scripts/test_pkg_call_kernel.py --filesystem

.PHONY: test-pkg-account-cancel-all-files
test-pkg-account-cancel-all-files:
	python3 scripts/test_pkg_cancel_recovery.py --account-all-files --block 4096
