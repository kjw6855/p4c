#ifndef _TOOLS_P4CE_P4_
#define _TOOLS_P4CE_P4_

/// Add @param check as a necessary path constraint for all subsequent executions in the program.
/// Enabled by default, unless `--disable-assumption-mode` is toggled.
extern void p4symbex_assume(in bool assumption);

/// Add @param check as a necessary path constraint for all subsequent executions in the program.
/// Enabled by default, unless `--disable-assumption-mode` is toggled.
/// If `--assertion-mode` is toggled, P4ce will not apply the condition to the path
/// constraints, but instead will try to only generate tests that violate this assumption.
extern void p4symbex_assert(in bool assumption);

#endif
