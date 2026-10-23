"""Per-program packages: programs/<target>/<arch>/<program>/ holds a program's capture scenario
(scenario.py), static-rule generator (static.py), exec.sh and the data they produced (snapshot/,
exploits/, validate/). Programs not packaged yet keep live/<program>.py and static/<program>.py.
"""
from __future__ import annotations

import importlib.util
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
PROGRAMS = HERE / "programs"


@dataclass(frozen=True)
class ControllerStep:
    """One controller action of a snapshot-sequence scenario (scenario.py STEPS): capture.py
    --sequence dumps the target after each, and validate_live.py runs it between an A2S case's
    packets."""
    run: Callable  # run(ctx): perform the step against the running target (after the setup)
    trigger: str = ""  # what the step does, recorded in the sequence
    # Registers whose cells this step's control plane writes (control-plane names, matched by
    # dotted suffix). Other register changes are data-plane writes by packets the step sends, and
    # stay out of the step's updates. Table changes always count. (Tofino dumps read the driver's
    # shadow copy, which data-plane writes do not reach; bmv2 dumps read the switch itself.)
    registers: Tuple[str, ...] = ()


def package_dir(program: str) -> Optional[Path]:
    found = sorted(d for d in PROGRAMS.glob(f"*/*/{program}") if d.is_dir())
    return found[0] if found else None


def _file(program: str, name: str, legacy: str) -> Path:
    pkg = package_dir(program)
    if pkg is not None and (pkg / name).exists():
        return pkg / name
    return HERE / legacy / f"{program}.py"


def scenario_path(program: str) -> Path:
    return _file(program, "scenario.py", "live")


def static_path(program: str) -> Path:
    return _file(program, "static.py", "static")


def static_programs() -> List[str]:
    """Every program with a static-rule generator, packaged or not."""
    names = {p.stem for p in (HERE / "static").glob("*.py")}
    names |= {p.parent.name for p in PROGRAMS.glob("*/*/*/static.py")}
    return sorted(names)


def load_module(path: Path, prefix: str):
    spec = importlib.util.spec_from_file_location(f"{prefix}_{abs(hash(str(path)))}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
