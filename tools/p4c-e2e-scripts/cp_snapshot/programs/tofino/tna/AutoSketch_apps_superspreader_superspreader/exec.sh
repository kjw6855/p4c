#!/usr/bin/env bash
# Live cp-snapshot capture, p4symbex and controller-in-the-loop validation of this package's
# program (the directory name): ./exec.sh --help. Runs cp_snapshot/package_exec.sh.
PKG=$(dirname "$(readlink -f "$0")")
exec bash "$PKG/../../../../package_exec.sh" "$(basename "$PKG")" "$@"
