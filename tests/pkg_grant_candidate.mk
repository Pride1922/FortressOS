# Standalone private gate; does not include the release Makefile.
.PHONY: test-pkg-grant-candidate
test-pkg-grant-candidate:
	python3 scripts/test_pkg_grant_candidate.py
