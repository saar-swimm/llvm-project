# BOLT

BOLT is a post-link optimizer developed to speed up large applications.
It achieves the improvements by optimizing application's code layout based on
execution profile gathered by sampling profiler, such as Linux `perf` tool.
An overview of the ideas implemented in BOLT along with a discussion of its
potential and current results is available in
[CGO'19 paper](https://research.fb.com/publications/bolt-a-practical-binary-optimizer-for-data-centers-and-beyond/).

## Input Binary Requirements

BOLT operates on X86-64 and AArch64 ELF binaries. At the minimum, the binaries
should have an unstripped symbol table, and, to get maximum performance gains,
they should be linked with relocations (`--emit-relocs` or `-q` linker flag).

BOLT disassembles functions and reconstructs the control flow graph (CFG)
before it runs optimizations. Since this is a nontrivial task,
especially when indirect branches are present, we rely on certain heuristics
to accomplish it. These heuristics have been tested on a code generated with
Clang and GCC compilers. The main requirement for C/C++ code is not to rely
on code layout properties, such as function pointer deltas.
Assembly code can be processed too. Requirements for it include a clear
separation of code and data, with data objects being placed into data
sections/segments. If indirect jumps are used for intra-function control
transfer (e.g., jump tables), the code patterns should be matching those
generated by Clang/GCC.

NOTE: BOLT is currently incompatible with the `-freorder-blocks-and-partition`
compiler option. Since GCC8 enables this option by default, you have to
explicitly disable it by adding `-fno-reorder-blocks-and-partition` flag if
you are compiling with GCC8 or above.

PIE and .so support has been added recently. Please report bugs if you
encounter any issues.

## Installation

### Docker Image

You can build and use the docker image containing BOLT using our [docker file](./bolt/utils/docker/Dockerfile).
Alternatively, you can build BOLT manually using the steps below.

### Manual Build

BOLT heavily uses LLVM libraries, and by design, it is built as one of LLVM
tools. The build process is not much different from a regular LLVM build.
The following instructions are assuming that you are running under Linux.

Start with cloning LLVM repo:

```
> git clone https://github.com/llvm/llvm-project.git
> mkdir build
> cd build
> cmake -G Ninja ../llvm-project/llvm -DLLVM_TARGETS_TO_BUILD="X86;AArch64" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_PROJECTS="bolt"
> ninja bolt
```

`llvm-bolt` will be available under `bin/`. Add this directory to your path to
ensure the rest of the commands in this tutorial work.

## Optimizing BOLT's Performance

BOLT runs many internal passes in parallel. If you foresee heavy usage of
BOLT, you can improve the processing time by linking against one of memory
allocation libraries with good support for concurrency. E.g. to use jemalloc:

```
> sudo yum install jemalloc-devel
> LD_PRELOAD=/usr/lib64/libjemalloc.so llvm-bolt ....
```
Or if you rather use tcmalloc:
```
> sudo yum install gperftools-devel
> LD_PRELOAD=/usr/lib64/libtcmalloc_minimal.so llvm-bolt ....
```

## Usage

For a complete practical guide of using BOLT see [Optimizing Clang with BOLT](./bolt/docs/OptimizingClang.md).

### Step 0

In order to allow BOLT to re-arrange functions (in addition to re-arranging
code within functions) in your program, it needs a little help from the linker.
Add `--emit-relocs` to the final link step of your application. You can verify
the presence of relocations by checking for `.rela.text` section in the binary.
BOLT will also report if it detects relocations while processing the binary.

### Step 1: Collect Profile

This step is different for different kinds of executables. If you can invoke
your program to run on a representative input from a command line, then check
**For Applications** section below. If your program typically runs as a
server/service, then skip to **For Services** section.

The version of `perf` command used for the following steps has to support
`-F brstack` option. We recommend using `perf` version 4.5 or later.

#### For Applications

This assumes you can run your program from a command line with a typical input.
In this case, simply prepend the command line invocation with `perf`:
```
$ perf record -e cycles:u -j any,u -o perf.data -- <executable> <args> ...
```

#### For Services

Once you get the service deployed and warmed-up, it is time to collect perf
data with LBR (branch information). The exact perf command to use will depend
on the service. E.g., to collect the data for all processes running on the
server for the next 3 minutes use:
```
$ perf record -e cycles:u -j any,u -a -o perf.data -- sleep 180
```

Depending on the application, you may need more samples to be included with
your profile. It's hard to tell upfront what would be a sweet spot for your
application. We recommend the profile to cover 1B instructions as reported
by BOLT `-dyno-stats` option. If you need to increase the number of samples
in the profile, you can either run the `sleep` command for longer and use
`-F<N>` option with `perf` to increase sampling frequency.

Note that for profile collection we recommend using cycle events and not
`BR_INST_RETIRED.*`. Empirically we found it to produce better results.

If the collection of a profile with branches is not available, e.g., when you run on
a VM or on hardware that does not support it, then you can use only sample
events, such as cycles. In this case, the quality of the profile information
would not be as good, and performance gains with BOLT are expected to be lower.

#### With instrumentation

If perf record is not available to you, you may collect profile by first
instrumenting the binary with BOLT and then running it.
```
llvm-bolt <executable> -instrument -o <instrumented-executable>
```

After you run instrumented-executable with the desired workload, its BOLT
profile should be ready for you in `/tmp/prof.fdata` and you can skip
**Step 2**.

Run BOLT with the `-help` option and check the category "BOLT instrumentation
options" for a quick reference on instrumentation knobs.

### Step 2: Convert Profile to BOLT Format

NOTE: you can skip this step and feed `perf.data` directly to BOLT using
experimental `-p perf.data` option.

For this step, you will need `perf.data` file collected from the previous step and
a copy of the binary that was running. The binary has to be either
unstripped, or should have a symbol table intact (i.e., running `strip -g` is
okay).

Make sure `perf` is in your `PATH`, and execute `perf2bolt`:
```
$ perf2bolt -p perf.data -o perf.fdata <executable>
```

This command will aggregate branch data from `perf.data` and store it in a
format that is both more compact and more resilient to binary modifications.

If the profile was collected without LBRs, you will need to add `-nl` flag to
the command line above.

### Step 3: Optimize with BOLT

Once you have `perf.fdata` ready, you can use it for optimizations with
BOLT. Assuming your environment is setup to include the right path, execute
`llvm-bolt`:
```
$ llvm-bolt <executable> -o <executable>.bolt -data=perf.fdata -reorder-blocks=cache+ -reorder-functions=hfsort -split-functions=2 -split-all-cold -split-eh -dyno-stats
```

If you do need an updated debug info, then add `-update-debug-sections` option
to the command above. The processing time will be slightly longer.

For a full list of options see `-help`/`-help-hidden` output.

The input binary for this step does not have to 100% match the binary used for
profile collection in **Step 1**. This could happen when you are doing active
development, and the source code constantly changes, yet you want to benefit
from profile-guided optimizations. However, since the binary is not precisely the
same, the profile information could become invalid or stale, and BOLT will
report the number of functions with a stale profile. The higher the
number, the less performance improvement should be expected. Thus, it is
crucial to update `.fdata` for release branches.

## Multiple Profiles

Suppose your application can run in different modes, and you can generate
multiple profiles for each one of them. To generate a single binary that can
benefit all modes (assuming the profiles don't contradict each other) you can
use `merge-fdata` tool:
```
$ merge-fdata *.fdata > combined.fdata
```
Use `combined.fdata` for **Step 3** above to generate a universally optimized
binary.

## License

BOLT is licensed under the [Apache License v2.0 with LLVM Exceptions](./LICENSE.TXT).
