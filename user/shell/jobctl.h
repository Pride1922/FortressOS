#ifndef SHELL_JOBCTL_H
#define SHELL_JOBCTL_H
/* Parent-shell builtins only; no lexer special case for ordinary % words. */
int jobctl_exec(int argc, const char *const *argv);
#endif
