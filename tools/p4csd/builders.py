"""Builder interface for target-specific compilation and test-case generation.

Each builder encapsulates the build and p4symbex stages for a specific target
(BMv2, Tofino, etc.), allowing the main pipeline to be target-agnostic for
steps 1 and 2 while remaining flexible for step 3 (testing).
"""

from abc import ABC, abstractmethod
from pathlib import Path
from typing import Callable, List, Tuple

# Forward declare for type hints; imported by caller
TargetSpec = None  # Populated by tampering.py at import time


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
        """Delegate to run_p4symbex() with p4_version from spec."""
        # Import here to avoid circular dependency
        from tampering import run_p4symbex

        protobuf_dir = output_root / spec.name / "protobuf"
        return run_p4symbex(
            spec.name,
            spec.p4_file,
            protobuf_dir,
            max_tests=args.max_tests,
            tamper_value=args.tamper_value,
            skip=args.skip_p4symbex,
            p4_version=spec.p4_version,
        )

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
        txtpb_files = sorted(protobuf_dir.glob("*.txtpb"))
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

        if use_cmake:
            artifact_dir = run_sde_cmake_build(spec.name, spec.p4_file, output_root)
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

        txtpb_dir = output_root / spec.name / "bfrt"
        p4symbex_bin = Path(getattr(args, "p4symbex_bin", None) or
                           __import__("tofino.tofino_driver", fromlist=["DEFAULT_P4SYMBEX_BIN"]).DEFAULT_P4SYMBEX_BIN)
        arch = getattr(args, "arch", "tna")

        return run_p4symbex(
            spec.name,
            spec.p4_file,
            txtpb_dir,
            arch=arch,
            max_tests=args.max_tests,
            tamper_value=args.tamper_value,
            skip=args.skip_p4symbex,
            p4symbex_bin=p4symbex_bin,
            p4_version=spec.p4_version,
        )

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
        txtpb_files = sorted(txtpb_dir.glob("*.txtpb"))
        if not txtpb_files:
            raise StepError(f"no .txtpb files found in {txtpb_dir}")
        # do_testing() doesn't use build_dir; pass the dir itself as a placeholder
        build_dir = output_root / spec.name / "build"
        return build_dir, build_dir, txtpb_files
