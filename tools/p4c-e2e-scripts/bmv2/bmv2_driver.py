"""BMv2 driver for the unified tampering harness.

The BMv2-specific build / p4symbex / test-replay logic currently lives inline in
`tampering.py` (the original test_bmv2_tampering.py rewritten for the unified
harness). This module re-exports the BMv2 helpers so callers can opt into the
"per-target subpackage" import style:

    from bmv2.bmv2_driver import run_bmv2_pipeline

The implementation has not been physically moved yet — see the in-progress
refactor described in
~/.claude/plans/support-the-new-delightful-wolf.md. For now this file is a
facade that exposes the canonical entry points by name.
"""

from __future__ import annotations

# Re-export the canonical BMv2 helpers via the top-level `tampering` module.
# `tampering.py` lives one directory up; the harness adds that directory to
# sys.path before dispatching, so a bare `import tampering` succeeds.
import tampering as _t  # noqa: E402

# Build / generate / test stage entry points. Names match the dispatcher in
# tampering.main().
do_build = _t.do_build
do_p4symbex = _t.do_p4symbex
do_testing = _t.do_testing

# Long-lived helpers (BMv2 process lifecycle, etc.).
Bmv2Process = _t.Bmv2Process
bmv2_session = _t.bmv2_session
# NOTE: the BMv2 control-plane client classes (P4RuntimeClient, P4RuntimeWriteException,
# Bmv2ThriftClient) are loaded lazily by tampering.load_bmv2_deps() and live as module
# globals on `tampering`. Do not re-export them here: capturing at import time would bind
# the pre-load None. Access them via `_t.<name>` after load if ever needed.


def run_bmv2_pipeline(args, csv_emit, status):
    """Run the full BMv2 pipeline (build → p4symbex → test) for the given args.

    This is currently identical to the inline `Pipeline.run()` invocation in
    tampering.main(); exposing it as a function lets the unified entry point
    dispatch cleanly across targets.
    """
    pipeline = _t.Pipeline(args.targets, args, csv_emit, status)
    pipeline.run()
