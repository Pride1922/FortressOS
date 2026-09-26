# Shell S7 Phase 3: flat pipeline grammar

**Status: COMPLETE (2026-09-26), user-confirmed.** Commit:
`fdcf13244a99ce1c0c032a7ee14a9e4db5f3ee24`.

The lexer distinguishes `TOK_PIPE` from `TOK_OR`; quoted and escaped pipes stay
literal. The existing static `parse_tree_t` retains its flat `parse_cmd_t[]`
layout and records `CMD_OP_PIPE` on each stage preceding a pipe. Eight stages
per consecutive pipe run are allowed; the total command-array limit remains
independent. No separate `parse_pipeline_t` was introduced.

`parser_execution_guard()` runs at the start of `execute_parse_tree`. Any pipe
in the tree rejects the entire chain with stderr diagnostic
`pipeline: not yet supported` and status 1, before commands, redirections or
spawns. Phase 4 removes this temporary guard and groups consecutive pipe markers
before applying chain operators. Phase 3 does not execute pipelines or resolve
their execution precedence.

Evidence reported by the user for this commit:

- `make test-shell-host`: 12/12 groups, including the new pipeline group.
- `make test-ext2`: 8/8 geometry cases.
- `make test-shell-s6`: BIOS and UEFI clean.
- Interactive canary: rejected pipeline does not open its redirection.

These results are recorded as user-reported evidence, not tests rerun during the
documentation update. The canary alone is not a process-count audit.

Follow-up: [Phase 4 implementation, awaiting user acceptance](shell-s7-phase4.md).
Its executor now replaces the temporary guard described at this historical checkpoint.

Phase 4's approved scope is pipeline execution for external programs on the
BSP only. Builtin stages and stream utilities are Phase 5 work; cross-core pipe
verification belongs to Phase 6. SIGPIPE and forced process cancellation remain
S8 work. These are future scope boundaries, not additional completed features.
