#!/usr/bin/env python3
# Copyright 2013-present Barefoot Networks, Inc.
# Copyright 2018 VMware, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import sys
import time
import subprocess
from pathlib import Path
from glob import glob
from .ebpfenv import Bridge
from .target import EBPFTarget
# path to the tools folder of the compiler
# Append tools to the import path.
FILE_DIR = Path(__file__).resolve().parent
# Append tools to the import path.
sys.path.append(str(FILE_DIR.joinpath("../../../tools")))
import testutils


class Target(EBPFTarget):
    EBPF_MAP_PATH = "/sys/fs/bpf/tc/globals"

    def __init__(self, tmpdir, options, template):
        EBPFTarget.__init__(self, tmpdir, options, template)

    def compile_dataplane(self):
        # Use clang to compile the generated C code to a LLVM IR
        args = "make "
        # target makefile
        args += f"-f {self.options.target}.mk "
        # Source folder of the makefile
        args += f"-C {self.runtimedir} "
        # Input eBPF byte code
        args += f"{self.template}.o "
        # The bpf program to attach to the interface
        args += f"BPFOBJ={self.template}.o "
        # add the folder local to the P4 file to the list of includes
        args += f" INCLUDES+=-I{os.path.dirname(self.options.p4filename)}"
        if self.options.extern:
            # we inline the extern so we need a direct include
            args += f" INCLUDES+=-include{self.options.extern} "
            # need to include the temporary dir because of the tmp import
            args += f" INCLUDES+=-I{self.tmpdir} "
        result = testutils.exec_process(args)
        if result.returncode != testutils.SUCCESS:
            testutils.log.error("Failed to compile the eBPF byte code")
        return result.returncode

    def _create_runtime(self):
        args = self.get_make_args(self.runtimedir, self.options.target)
        # List of bpf programs to attach to the interface
        args += f"BPFOBJ={self.template} "
        args += "CFLAGS+=-DCONTROL_PLANE "
        # add the folder local to the P4 file to the list of includes
        args += f"INCLUDES+=-I{os.path.dirname(self.options.p4filename)} "
        # some kernel specific includes for libbpf
        args += f"INCLUDES+=-I{self.runtimedir}/usr/include "
        args += f"INCLUDES+=-I{self.runtimedir}/contrib/libbpf/include/uapi "
        args += f"LIBS+={self.runtimedir}/usr/lib64/libbpf.a "
        args += "LIBS+=-lz "
        args += "LIBS+=-lelf "
        result = testutils.exec_process(args)
        if result.returncode != testutils.SUCCESS:
            testutils.log.error("Failed to build the filter")
        return result.returncode

    def _create_bridge(self):
        # The namespace is the id of the process
        namespace = str(os.getpid())
        # Number of input files
        direction = "in"
        num_files = len(glob(self.filename("*", direction)))
        # Create the namespace and the bridge with all its ports
        br = Bridge(namespace)
        result = br.create_virtual_env(num_files)
        if result != testutils.SUCCESS:
            br.ns_del()
            return None
        return br

    def _get_run_cmd(self):
        direction = "in"
        pcap_pattern = self.filename("", direction)
        num_files = len(glob(self.filename("*", direction)))
        testutils.log.info("Input file: %s", pcap_pattern)
        # Main executable
        cmd = self.template + " "
        # Input pcap pattern
        cmd += "-f " + pcap_pattern + " "
        # Number of input interfaces
        cmd += "-n " + str(num_files) + " "
        # Debug flag (verbose output)
        cmd += "-d"
        return cmd

    def _kill_processes(self, procs):
        for proc in procs:
            # kill process, 15 is SIGTERM
            os.kill(proc.pid, 15)

    def _load_filter(self, bridge, proc, port_name):
        # Load the specified eBPF object to "port_name" egress
        # As a side-effect, this may create maps in /sys/fs/bpf/

        # Add the qdisc. MUST be clsact layer.
        bridge.ns_exec(f"tc qdisc add dev {port_name} clsact")
        cmd = (f"tc filter add dev {port_name} egress"
               f" bpf da obj {self.template}.o section prog verbose")
        return bridge.ns_proc_write(proc, cmd)

    def _attach_filters(self, bridge, proc):
        # Get the command to load eBPF code to all the attached ports
        if len(bridge.edge_ports) > 0:
            for port in bridge.edge_ports:
                result = self._load_filter(bridge, proc, port)
                bridge.ns_proc_append(proc, "")
        else:
            # No ports attached (no pcap files), load to bridge instead
            result = self._load_filter(bridge, proc, bridge.br_name)
            bridge.ns_proc_append(proc, "")
        if result != testutils.SUCCESS:
            return result
        return testutils.SUCCESS

    def _run_tcpdump(self, bridge, filename, port):
        cmd = f"{bridge.get_ns_prefix()} tcpdump -w {filename} -i {port}"
        return subprocess.Popen(cmd.split())

    def _init_tcpdump_listeners(self, bridge):
        # Listen to packets with tcpdump on all the ports of the bridge
        dump_procs = []
        for i, port in enumerate(bridge.br_ports):
            outfile_name = self.filename(i, "out")
            dump_procs.append(self._run_tcpdump(bridge, outfile_name, port))
        # Wait for tcpdump to initialise
        time.sleep(2)
        return dump_procs

    def _run_in_namespace(self, bridge):
        # Open a process in the new namespace
        proc = bridge.ns_proc_open()
        if not proc:
            return testutils.FAILURE
        dump_procs = self._init_tcpdump_listeners(bridge)
        result = self._attach_filters(bridge, proc)
        if result != testutils.SUCCESS:
            return result
        # Check if eBPF maps have actually been created
        result = bridge.ns_proc_write(proc, f"ls -1 {self.EBPF_MAP_PATH}")
        if result != testutils.SUCCESS:
            return result
        # Finally, append the actual runtime command to the process
        result = bridge.ns_proc_append(proc, self._get_run_cmd())
        if result != testutils.SUCCESS:
            return result
        # Execute the command queue and close the process, retrieve result
        result = bridge.ns_proc_close(proc)
        # Kill tcpdump but let it finish writing packets
        self._kill_processes(dump_procs)
        time.sleep(2)
        return result

    def run(self):
        # Root is necessary to load ebpf into the kernel
        if not testutils.check_root():
            testutils.log.warning("This test requires root privileges; skipping execution.")
            return testutils.SKIPPED
        result = self._create_runtime()
        if result != testutils.SUCCESS:
            return result
        # Create the namespace and the central testing bridge
        bridge = self._create_bridge()
        if not bridge:
            return testutils.FAILURE
        # Run the program in the generated namespace
        result = self._run_in_namespace(bridge)
        bridge.ns_del()
        return result
