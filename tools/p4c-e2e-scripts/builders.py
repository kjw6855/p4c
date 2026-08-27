"""Builder interface for target-specific compilation and test-case generation.

Each builder encapsulates the build and p4symbex stages for a specific target
(BMv2, Tofino, etc.), allowing the main pipeline to be target-agnostic for
steps 1 and 2 while remaining flexible for step 3 (testing).
"""

import logging
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Callable, List, Tuple

log = logging.getLogger("builders")

# Forward declare for type hints; imported by caller
TargetSpec = None  # Populated by tampering.py at import time


def _cache_file_for(args, spec):
    """Path of the pre-computed SOChain cache for @spec, or None when --state-dep-cache-root is
    unset. The cache is GLOBAL per (target, arch, program) — one file holds both Key and Cond chains
    and is shared by every tamper-mode policy. When set, p4symbex hard-errors if the file is missing
    or stale, so a missing cache surfaces as that program's error rather than silently recomputing."""
    root = getattr(args, "state_dep_cache_root", None)
    if not root:
        return None
    return Path(root).expanduser() / spec.target / spec.arch / f"{spec.name}.chains"


def _cp_annotation_for(args, spec):
    """Path of the external control-plane / port annotation for @spec, or None.

    Unlike the SOChain cache this is OPTIONAL data: a program with no annotation file simply runs
    unannotated (p4symbex only hard-errors when the flag is given and the file is unreadable), so a
    missing file is not an error. Flat layout - one JSON per program name, no target/arch split,
    because the annotation describes the program and its control plane, not the compile target."""
    root = getattr(args, "cp_annotation_root", None)
    if not root:
        return None
    path = Path(root).expanduser() / f"{spec.name}.json"
    return path if path.exists() else None


def _gen_per_policy(base: Path, policies: List[str], skip: bool,
                    run_one: Callable[[Path, str], List[Path]], name: str) -> List[Path]:
    """Run p4symbex once per --tamper-mode policy into ``base/<policy>/`` and return all .txtpb.

    Each policy gets its own subdir because p4symbex restarts filenames at chain 0 per policy, so
    a flat layout would let the second policy overwrite the first. A per-policy failure (e.g. one
    policy times out) is logged and skipped so the other policy's output is still kept."""
    if skip and base.exists():
        existing = sorted(base.rglob("*.txtpb"))
        if existing:
            log.info("[%s] reusing %d existing txtpb under %s", name, len(existing), base)
            return existing
    # Fresh run: clear stale outputs so the result is an unambiguous reflection of this run.
    #
    # Scoped to the policies actually being generated, plus any file left directly under `base` by
    # the old flat layout. A blanket base.rglob() would also wipe the OTHER policy's subdir -- and
    # since a timeout kills one policy while the other succeeds (see the except below), a targeted
    # re-run of the failed half would silently destroy the good half it was never asked to touch.
    if base.exists():
        for f in base.glob("*.txtpb"):          # legacy flat layout
            f.unlink()
        for pol in policies:
            pol_dir = base / pol
            if pol_dir.is_dir():
                for f in pol_dir.rglob("*.txtpb"):
                    f.unlink()
    all_files: List[Path] = []
    for pol in policies:
        try:
            all_files.extend(run_one(base / pol, pol))
        except Exception as ex:  # keep the other policy's output on a per-policy failure
            log.warning("[%s] p4symbex policy %s failed: %s", name, pol, ex)
    return all_files


class BuilderBase(ABC):
    """Abstract base class for target-specific builders.

    Each builder is responsible for:
      - build():    compiling a P4 program to its target artifacts
      - p4symbex(): generating symbolic execution test cases as protobufs
      - test():     replaying the generated cases against the target switch
    """

    @abstractmethod
    def build(self, spec: "TargetSpec", output_root: Path, args) -> Tuple[Path, Path]:
        """Build a P4 program.

        Args:
            spec: TargetSpec with p4_file, p4_version, extra_args
            output_root: Root directory for build outputs
            args: Parsed command-line arguments

        Returns:
            (json_path, p4info_path): Tuple of output file paths

        Raises:
            StepError: On compilation failure
        """
        pass

    @abstractmethod
    def p4symbex(self, spec: "TargetSpec", output_root: Path, args) -> List[Path]:
        """Generate symbolic execution test cases.

        Args:
            spec: TargetSpec with p4_file, p4_version, extra_args
            output_root: Root directory for test outputs
            args: Parsed command-line arguments (max_tests, tamper_value, skip_p4symbex)

        Returns:
            List of .txtpb protobuf file paths

        Raises:
            StepError: On test generation failure or if no tests produced
        """
        pass

    @abstractmethod
    def test(self, spec: "TargetSpec", artifacts: Tuple[Path, Path],
             txtpb_files: List[Path], args,
             csv_writer: Callable[[str, str, str, str], None],
             progress_cb: Callable[[int, int, str], None]
            ) -> Tuple[int, int, int]:
        """Replay generated test cases against the target's switch model.

        Args:
            spec:         TargetSpec for the program.
            artifacts:    Whatever build() returned (BMv2: (json, p4info);
                          Tofino: (bfrt.json, context.json)).
            txtpb_files:  List of .txtpb files emitted by p4symbex().
            args:         Parsed CLI args.
            csv_writer:   csv_emit(target, protobuf_file, result, reason).
            progress_cb:  progress_cb(idx, total, current_txtpb_name).

        Returns:
            (ok, fail, err) protobuf-granularity counters.
        """
        pass

    @abstractmethod
    def find_artifacts(self, spec: "TargetSpec", output_root: Path, args) -> Tuple[Path, Path, List[Path]]:
        """Locate pre-existing build and test-case artifacts for --stage test.

        Returns:
            (json_or_bfrt_path, p4info_or_context_path, txtpb_files)

        Raises:
            StepError: if any expected artifact is missing.
        """
        pass


class BMv2Builder(BuilderBase):
    """Builder for BMv2 targets (simple_switch_grpc with v1model)."""

    def build(self, spec: "TargetSpec", output_root: Path, args) -> Tuple[Path, Path]:
        """Delegate to run_p4c() with p4_version from spec."""
        # Import here to avoid circular dependency
        from tampering import run_p4c

        build_dir = output_root / spec.name / "build"
        return run_p4c(
            spec.name,
            spec.p4_file,
            spec.extra_args,
            build_dir,
            p4_version=spec.p4_version,
        )

    def p4symbex(self, spec: "TargetSpec", output_root: Path, args) -> List[Path]:
        """Generate protobuf test cases, once per --tamper-mode policy (Write-Key / -Condition)."""
        # Import here to avoid circular dependency
        from tampering import run_p4symbex, TAMPER_MODE_POLICIES

        base = output_root / spec.name / "protobuf"
        return _gen_per_policy(
            base, TAMPER_MODE_POLICIES[args.tamper_mode], args.skip_p4symbex,
            lambda sub, pol: run_p4symbex(
                spec.name, spec.p4_file, sub,
                max_tests=args.max_tests, tamper_value=args.tamper_value,
                skip=False, p4_version=spec.p4_version,
                extra_args=spec.extra_args, path_selection=pol,
                state_dep_cache=_cache_file_for(args, spec),
                cp_annotation=_cp_annotation_for(args, spec),
                shared_traversal=getattr(args, "shared_traversal", None),
                chain_impact_order=getattr(args, "chain_impact_order", False),
                timeout=spec.timeout),
            spec.name)

    def test(self, spec, artifacts, txtpb_files, args, csv_writer, progress_cb):
        """Delegate to tampering.do_testing — the existing BMv2 path."""
        from tampering import do_testing
        json_path, p4info_path = artifacts
        return do_testing(spec, json_path, p4info_path, txtpb_files,
                          args, csv_writer, progress_cb)

    def find_artifacts(self, spec, output_root, args):
        from tampering import StepError
        build_dir = output_root / spec.name / "build"
        protobuf_dir = output_root / spec.name / "protobuf"
        json_path = build_dir / f"{spec.name}.json"
        p4info_path = build_dir / f"{spec.name}_p4info.txt"
        if not json_path.exists():
            raise StepError(f"build artifact not found: {json_path}")
        if not p4info_path.exists():
            raise StepError(f"build artifact not found: {p4info_path}")
        txtpb_files = sorted(protobuf_dir.rglob("*.txtpb"))  # rglob: per-policy subdirs + legacy flat
        if not txtpb_files:
            raise StepError(f"no .txtpb files found in {protobuf_dir}")
        return json_path, p4info_path, txtpb_files


class TofinoBuilder(BuilderBase):
    """Builder for Tofino targets (tofino_model with TNA).

    Delegates to tofino_driver.py for build and p4symbex stages, with p4_version
    parameterization support for dynamic P4 standard selection.
    """

    def build(self, spec: "TargetSpec", output_root: Path, args) -> Tuple[Path, Path]:
        """Build a Tofino TNA program via tofino_driver.run_p4c() or cmake.

        When --sde-cmake-build is set, delegates to run_sde_cmake_build() which
        installs the program under $SDE_INSTALL/share/tofinopd/<name>/ via the
        SDE p4studio cmake workflow — no local p4c binary required.

        Returns (bf_rt_json_path, context_json_path) as the artifact tuple,
        mirroring the BMv2 interface (json_path, p4info_path).
        """
        from tofino.tofino_driver import (
            run_p4c, run_sde_cmake_build, get_tofino_artifacts,
            DEFAULT_P4C_BIN,
        )

        arch = getattr(args, "arch", "tna")
        use_cmake = getattr(args, "sde_cmake_build", False)
        # Both compile paths skip when prior artifacts exist, and neither looks at whether the
        # .p4 (or anything it #includes) is newer. --force-compile is the only way to rebuild
        # after editing a source; without it an edited program silently replays its old binary.
        force = getattr(args, "force_compile", False)

        if use_cmake:
            artifact_dir = run_sde_cmake_build(spec.name, spec.p4_file, output_root,
                                               extra_args=spec.extra_args, force=force)
        else:
            build_dir = output_root / spec.name / "build"
            p4c_bin = Path(getattr(args, "p4c_bin", None) or DEFAULT_P4C_BIN)
            run_p4c(
                spec.name,
                spec.p4_file,
                spec.extra_args,
                build_dir,
                arch=arch,
                p4c_bin=p4c_bin,
                p4_version=spec.p4_version,
                force=force,
            )
            artifact_dir = build_dir

        bfrt_path, context_path = get_tofino_artifacts(artifact_dir)
        if bfrt_path is None or context_path is None:
            raise RuntimeError(
                f"Could not find Tofino artifacts in {artifact_dir}: "
                "Expected bf-rt.json and pipe/context.json."
            )
        return bfrt_path, context_path

    def p4symbex(self, spec: "TargetSpec", output_root: Path, args) -> List[Path]:
        """Generate Tofino BFRT test cases via tofino_driver.run_p4symbex().

        Returns list of .txtpb protobuf file paths for the tampering test cases.
        """
        # Import here to avoid circular dependency
        from tofino.tofino_driver import run_p4symbex
        from tampering import TAMPER_MODE_POLICIES

        base = output_root / spec.name / "bfrt"
        p4symbex_bin = Path(getattr(args, "p4symbex_bin", None) or
                           __import__("tofino.tofino_driver", fromlist=["DEFAULT_P4SYMBEX_BIN"]).DEFAULT_P4SYMBEX_BIN)
        arch = getattr(args, "arch", "tna")

        return _gen_per_policy(
            base, TAMPER_MODE_POLICIES[args.tamper_mode], args.skip_p4symbex,
            lambda sub, pol: run_p4symbex(
                spec.name, spec.p4_file, sub,
                arch=arch, max_tests=args.max_tests, tamper_value=args.tamper_value,
                skip=False, p4symbex_bin=p4symbex_bin, p4_version=spec.p4_version,
                extra_args=spec.extra_args, path_selection=pol,
                state_dep_cache=_cache_file_for(args, spec),
                cp_annotation=_cp_annotation_for(args, spec),
                shared_traversal=getattr(args, "shared_traversal", None),
                chain_impact_order=getattr(args, "chain_impact_order", False),
                timeout=spec.timeout),
            spec.name)

    def test(self, spec, artifacts, txtpb_files, args, csv_writer, progress_cb):
        """Delegate to tofino_driver.do_testing."""
        from tofino.tofino_driver import do_testing
        build_dir = artifacts[0].parent if isinstance(artifacts, tuple) else artifacts
        manage_procs = getattr(args, "sde_manage_procs", False)
        return do_testing(spec, build_dir, txtpb_files, args, csv_writer, progress_cb,
                          manage_procs=manage_procs)

    def find_artifacts(self, spec, output_root, args):
        from tampering import StepError
        txtpb_dir = output_root / spec.name / "bfrt"
        txtpb_files = sorted(txtpb_dir.rglob("*.txtpb"))  # rglob: per-policy subdirs + legacy flat
        if not txtpb_files:
            raise StepError(f"no .txtpb files found in {txtpb_dir}")
        # do_testing() doesn't use build_dir; pass the dir itself as a placeholder
        build_dir = output_root / spec.name / "build"
        return build_dir, build_dir, txtpb_files
