#!/bin/bash
# Copyright 2025 Google LLC
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
function extract_workload_name() {
  local workload_string="$1"
  echo "${workload_string##*.}"
}
function is_workload_selected() {
  local workload="$1"
  local -a selected_list
  local index=0
  for arg in "${@:2}"; do
    selected_list[index]="$arg"
    ((index++))
  done
  for item in "${selected_list[@]}"; do
    if [[ "$workload" == "$item" ]]; then
      return 0
    fi
  done
  return 1
}
function validate_spec_size() {
  local option_name="$1"
  local size="$2"
  case "$size" in
    test|train|ref)
      return 0
      ;;
    *)
      echo "ERROR: ${option_name} must be one of: test, train, ref." >&2
      return 1
      ;;
  esac
}
function find_latest_spec_run_dir() {
  local benchmark_dir="$1"
  local size="$2"
  local workload="$3"
  local run_root="${benchmark_dir}/run"
  local run_size="$size"
  local candidate
  local candidate_mtime
  local latest_dir=""
  local latest_mtime=-1
  if [[ "$size" == "ref" ]]; then
    if [[ "$workload" == *_r ]]; then
      run_size="refrate"
    elif [[ "$workload" == *_s ]]; then
      run_size="refspeed"
    fi
  fi
  while IFS= read -r -d $'\0' candidate; do
    candidate_mtime=$(stat -c '%Y' "$candidate") || continue
    if (( candidate_mtime > latest_mtime )); then
      latest_mtime=$candidate_mtime
      latest_dir="$candidate"
    fi
  done < <(
    find "$run_root" \
      -mindepth 1 \
      -maxdepth 1 \
      -type d \
      -name "run_base_${run_size}_mytest-m64.*" \
      -print0 2>/dev/null
  )
  if [[ -z "$latest_dir" ]]; then
    echo "ERROR: Could not find a ${size} run directory under: ${run_root}" >&2
    echo "       Expected pattern: run_base_${run_size}_mytest-m64.*" >&2
    return 1
  fi
  printf '%s\n' "$latest_dir"
}
function process_workload_memory_profiles() {
  local profile_dir="$1"
  local workload="$2"
  local binary_path="$3"
  local profile_file=""
  local largest_size=-1
  local file_size
  while IFS= read -r -d $'\0' candidate; do
    file_size=$(stat -c '%s' "$candidate") || continue
    if (( file_size > largest_size )); then
      largest_size=$file_size
      profile_file="$candidate"
    fi
  done < <(
    find "$profile_dir" \
      -maxdepth 1 \
      -type f \
      -name "memprof.profraw.*" \
      -print0
  )
  if [[ -z "$profile_file" ]]; then
    echo "ERROR: No MemProf profiles found for workload: $workload" >&2
    echo "       Profile directory: $profile_dir" >&2
    return 1
  fi
  if [[ -z "${OUT:-}" ]]; then
    echo "ERROR: Output directory is not set." >&2
    return 1
  fi
  if ! mkdir -p -- "$OUT"; then
    echo "ERROR: Could not create output directory: $OUT" >&2
    return 1
  fi
  local full_command="$base_command \"$binary_path\" --memprof_profile \"$profile_file\""
  local typetree="$OUT/${workload}.typetree"
  local verbose="$OUT/${workload}.verbose"
  local stats="$OUT/${workload}.stats"
  local unresolved="$OUT/${workload}.unresolved"
  echo "Using largest MemProf profile: $profile_file (${largest_size} bytes)"
  # Dump-only mode: skip the full typetree/stats run entirely and only
  # produce the unresolved-callstacks dump.
  if [[ "$dump_unresolved_only" == true ]]; then
    echo "$full_command --dump_unresolved_callstacks > $unresolved 2> /dev/null"
    eval "$full_command --dump_unresolved_callstacks" \
      > "$unresolved" \
      2> /dev/null
    echo "-----------------------------------------"
    echo "Finished processing workload: $workload (dump-only unresolved callstacks)"
    if [[ -s "$unresolved" ]]; then
      echo "Unresolved callstacks written to: $unresolved"
    else
      echo "WARNING: No unresolved callstack output produced for $workload" >&2
    fi
    echo "========================================="
    return 0
  fi
  echo "$full_command > $typetree 2> $verbose"
  # Save stdout to typetree. Save stderr to verbose and print it to the terminal.
  eval "$full_command" \
    > "$typetree" \
    2> >(tee "$verbose" >&2)
  awk '
    /====== Statistics ======/ {
      if (!capturing) {
        capturing = 1
      }
    }
    capturing {
      print
    }
    capturing && /====== End Statistics ======/ {
      exit
    }
  ' "$verbose" > "$stats"
  if [[ "$dump_unresolved_callstacks" == true ]]; then
    echo "$full_command --dump_unresolved_callstacks > $unresolved 2> /dev/null"
    eval "$full_command --dump_unresolved_callstacks" \
      > "$unresolved" \
      2> /dev/null
  fi
  echo "-----------------------------------------"
  echo "Finished processing workload: $workload"
  if [[ -s "$stats" ]]; then
    cat "$stats"
  fi
  echo "========================================="
}
# ============ SPEC CPU 2017 ====================
spec_workloads=(
  "505.mcf_r"
  "508.namd_r"
  "510.parest_r"
  "519.lbm_r"
  "523.xalancbmk_r"
  "526.blender_r"
  "541.leela_r"
  "544.nab_r"
)
build_spec() {
  echo "Building SPEC CPU 2017 with input size: ${SPEC17_SIZE}..."
  find "${SPEC_DIR}" \
    \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -exec sed -i \
      "s|(s\.c_str() != '\\\\0') || (\*endptr == '\\\\0')|(!s.empty() || (*endptr == '\\0'))|g" \
      {} +
  cp "${TOP_DIR}/cfg/memprof.cfg" \
    "${SPEC_DIR}/config/memprof.cfg"
  sed -i \
    "s|^[[:space:]]*LLVM_PATH[[:space:]]*=.*|LLVM_PATH = ${TOP_DIR}/third_party/llvm-project/|" \
    "${SPEC_DIR}/config/memprof.cfg"
  cd "${SPEC_DIR}" || return 1
  source shrc
  for workload in "${spec_workloads[@]}"; do
    if [[ "${benchmark_provided}" == true ]] &&
       ! is_workload_selected \
         "${workload}" "${selected_benchmarks[@]}"; then
      continue
    fi
    echo "Scrub workload: ${workload}"
    build_cmd="runcpu --config=memprof --action=scrub ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
    echo "Building workload: ${workload}"
    build_cmd="runcpu --config=memprof --action=build ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
    echo "Running and collecting ${SPEC17_SIZE} memory profiles for: ${workload}"
    build_cmd="runcpu --config=memprof --action=run --copies=1 --iterations=1 --tune=base --size=${SPEC17_SIZE} ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
  done
}
run_spec() {
  echo "Processing SPEC CPU 2017 ${SPEC17_SIZE} workloads..."
  for workload in "${spec_workloads[@]}"; do
    if ! is_workload_selected "${workload}" "$@"; then
      continue
    fi
    echo "Processing workload: $workload"
    local workload_name
    local benchmark_dir
    local profile_dir
    local binary_path
    workload_name=$(extract_workload_name "$workload")
    if [[ "${workload_name}" == xalancbmk* ]]; then
      workload_name="cpuxalan_r"
    fi
    benchmark_dir="$SPEC_DIR/benchspec/CPU/$workload"
    if ! profile_dir=$(
      find_latest_spec_run_dir "$benchmark_dir" "$SPEC17_SIZE" "$workload"
    ); then
      continue
    fi
    binary_path="$profile_dir/${workload_name}_base.mytest-m64"
    if [[ ! -f "$binary_path" ]]; then
      binary_path=$(find "$profile_dir" \
        -maxdepth 1 \
        -type f \
        -name "*_base.mytest-m64" \
        -perm -u+x \
        -print \
        -quit)
    fi
    if [[ -z "$binary_path" || ! -f "$binary_path" ]]; then
      echo "ERROR: Could not find binary for SPEC CPU 2017 workload: $workload" >&2
      echo "       Profile directory: $profile_dir" >&2
      continue
    fi
    echo "Using run directory: $profile_dir"
    echo "Using binary: $binary_path"
    process_workload_memory_profiles "$profile_dir" "$workload_name" "$binary_path"
  done
  echo "Finished processing all specified SPEC CPU 2017 workloads."
}
# ============ SPEC CPU 2026 ====================
spec26_workloads=(
  "706.stockfish_r"
  "707.ntest_r"
  "708.sqlite_r"
  "710.omnetpp_r"
  "714.cpython_r"
  "721.gcc_r"
  "723.llvm_r"
  # "727.cppcheck_r"
  "729.abc_r"
  "734.vpr_r"
  "750.sealcrypto_r"
  "753.ns3_r"
  "777.zstd_r"
  "801.xz_s"
  "817.flac_s"
  "838.diamond_s"
  "846.minizinc_s"
  # "854.graph500_s"
  # "709.cactus_r"
  # "731.astcenc_r"
  "736.ocio_r"
  # "737.gmsh_r"
  # "766.femflow_r"
  "772.marian_r"
  # "735.gem5_r"
  # "803.sph_exa_s"
  # "811.tealeaf_s"
  # "857.namd_s"
  # "881.neutron_s"
)
build_spec26() {
  echo "Building SPEC CPU 2026 with input size: ${SPEC26_SIZE}..."
  cp "${TOP_DIR}/cfg/memprof26.cfg" \
    "${SPEC_26_DIR}/config/memprof26.cfg"
  sed -i \
    "s|^[[:space:]]*LLVM_PATH[[:space:]]*=.*|LLVM_PATH = ${TOP_DIR}/third_party/llvm-project/|" \
    "${SPEC_26_DIR}/config/memprof26.cfg"
  cd "${SPEC_26_DIR}" || return 1
  source shrc
  for workload in "${spec26_workloads[@]}"; do
    if [[ "${benchmark_provided}" == true ]] &&
       ! is_workload_selected \
         "${workload}" "${selected_benchmarks[@]}"; then
      continue
    fi
    echo "Scrub workload: ${workload}"
    build_cmd="runcpu --config=memprof26 --action=scrub ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
    echo "Building workload: ${workload}"
    build_cmd="runcpu --config=memprof26 --action=build ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
    echo "Running and collecting ${SPEC26_SIZE} memory profiles for: ${workload}"
    build_cmd="runcpu --config=memprof26 --action=run --copies=1 --iterations=1 --tune=base --size=${SPEC26_SIZE} ${workload}"
    echo "${build_cmd}"
    eval "${build_cmd}"
  done
}
run_spec26() {
  echo "Processing SPEC CPU 2026 ${SPEC26_SIZE} workloads..."
  for workload in "${spec26_workloads[@]}"; do
    if ! is_workload_selected "${workload}" "$@"; then
      continue
    fi
    echo "Processing workload: $workload"
    local workload_name
    local benchmark_dir
    local profile_dir
    local binary_path
    workload_name=$(extract_workload_name "$workload")
    benchmark_dir="$SPEC_26_DIR/benchspec/CPU/$workload"
    if ! profile_dir=$(
      find_latest_spec_run_dir "$benchmark_dir" "$SPEC26_SIZE" "$workload"
    ); then
      continue
    fi
    binary_path="$profile_dir/${workload_name}_base.mytest-m64"
    if [[ ! -f "$binary_path" ]]; then
      binary_path=$(find "$profile_dir" \
        -maxdepth 1 \
        -type f \
        -name "*_base.mytest-m64" \
        -perm -u+x \
        -print \
        -quit)
    fi
    if [[ -z "$binary_path" || ! -f "$binary_path" ]]; then
      echo "ERROR: Could not find binary for SPEC CPU 2026 workload: $workload" >&2
      echo "       Profile directory: $profile_dir" >&2
      continue
    fi
    echo "Using run directory: $profile_dir"
    echo "Using binary: $binary_path"
    process_workload_memory_profiles "$profile_dir" "$workload_name" "$binary_path"
  done
  echo "Finished processing all specified SPEC CPU 2026 workloads."
}
# ============ CLANG ====================
clang_workloads=(
  "input1.cpp"
)
build_clang() {
  echo "Building Clang..."
  cd "${TESTSUITE_DIR}/clang-memprof" || return 1
  mkdir -p build && cd build || return 1
  cmake -GNinja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="${LLVM_BIN_DIR}/clang" \
    -DCMAKE_CXX_COMPILER="${LLVM_BIN_DIR}/clang++" \
    -DCMAKE_LINKER="${LLVM_BIN_DIR}/lld" \
    -DLLVM_ENABLE_PROJECTS="clang" \
    -DLLVM_TARGETS_TO_BUILD="host" \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DLLVM_ENABLE_LLD=ON \
    -DCMAKE_C_FLAGS="-O3 -g -fuse-ld=lld -Wl,--no-rosegment -fno-exceptions -fdebug-info-for-profiling -fPIC -mno-omit-leaf-frame-pointer -fno-omit-frame-pointer -fno-optimize-sibling-calls -m64 -Wl,-build-id -no-pie -fPIC -fmemory-profile=/tmp -fprofile-generate=/tmp -mllvm -memprof-histogram" \
    -DCMAKE_CXX_FLAGS="-O3 -g -fuse-ld=lld -Wl,--no-rosegment -fno-exceptions -fdebug-info-for-profiling -fPIC -mno-omit-leaf-frame-pointer -fno-omit-frame-pointer -fno-optimize-sibling-calls -m64 -Wl,-build-id -no-pie -fPIC -fmemory-profile=/tmp -fprofile-generate=/tmp -mllvm -memprof-histogram" \
    -DCMAKE_EXE_LINKER_FLAGS="-flto=thin -Wl,-O3 \
    -fprofile-generate=/tmp -fmemory-profile=/tmp \
    -L/home/wmatt/llvm-memprof/third_party/llvm-project/install/lib \
    -Wl,-rpath,/home/wmatt/llvm-memprof/third_party/llvm-project/install/lib \
    -l:libclang_rt.memprof.a" \
    -DCMAKE_SHARED_LINKER_FLAGS="-flto=thin -Wl,-O3 \
    -fprofile-generate=/tmp -fmemory-profile=/tmp \
    -L/home/wmatt/llvm-memprof/third_party/llvm-project/install/lib \
    -Wl,-rpath,/home/wmatt/llvm-memprof/third_party/llvm-project/install/lib \
    -l:libclang_rt.memprof.a" \
    ../llvm
  echo "Starting clang build"
  ninja -j "$(nproc)"
  echo "Clang build completed."
}
run_clang() {
  echo "Processing CLANG workloads..."
  local CLANG_BIN="${TESTSUITE_DIR}/clang-memprof/build/bin"
  local CLANG_WL="${TESTSUITE_DIR}/clang-inputs"
  rm -f ./*.profraw ./*.profraw.*
  echo "Running clang with input1.cpp"
  echo "${CLANG_BIN}/clang ${CLANG_WL}/input1.cpp -std=c++14 -O2 -c -o /tmp/input1.o"
  "${CLANG_BIN}/clang" "${CLANG_WL}/input1.cpp" \
    -std=c++14 -O2 -c -o /tmp/input1.o
  process_workload_memory_profiles "$(pwd)" "input1" "${CLANG_BIN}/clang"
  echo "Finished processing CLANG workloads."
}
# ============ FleetBench ====================
fleetbench_workloads=(
  "proto_benchmark"
  "swissmap_benchmark"
  # "mem_benchmark"
  "empirical_driver"
  "compression_benchmark"
  "hashing_benchmark"
  "cord_benchmark"
  "rpc_benchmark"
)
function run_fleetbench() {
  echo "Processing FleetBench workloads..."
  local fleetbench_dir="${TOP_DIR}/testsuite/fleetbench"
  local llvm_bin_dir="${TOP_DIR}/third_party/llvm-project/install/bin"
  local memprof_clang="${llvm_bin_dir}/clang"
  local memprof_clangxx="${llvm_bin_dir}/clang++"
  local bazel_cmd="bazel-8.0.0"
  if [[ ! -d "${fleetbench_dir}" ]]; then
    echo "ERROR: FleetBench directory not found: ${fleetbench_dir}" >&2
    return 1
  fi
  if [[ ! -f "${fleetbench_dir}/MODULE.bazel" ]]; then
    echo "ERROR: MODULE.bazel not found in FleetBench directory:" >&2
    echo "       ${fleetbench_dir}" >&2
    return 1
  fi
  if ! command -v "${bazel_cmd}" >/dev/null 2>&1; then
    echo "ERROR: ${bazel_cmd} not found in PATH." >&2
    return 1
  fi
  if [[ ! -x "${memprof_clang}" ]]; then
    echo "ERROR: MemProf clang not found: ${memprof_clang}" >&2
    return 1
  fi
  if [[ ! -x "${memprof_clangxx}" ]]; then
    echo "ERROR: MemProf clang++ not found: ${memprof_clangxx}" >&2
    return 1
  fi
  echo "FleetBench directory: ${fleetbench_dir}"
  echo "FleetBench Bazel:     ${bazel_cmd}"
  echo "FleetBench CC:        ${memprof_clang}"
  echo "FleetBench CXX:       ${memprof_clangxx}"
  echo "FleetBench allocator: C++ system allocator"
  local -a benchmark_targets=(
    "//fleetbench/proto:proto_benchmark"
    "//fleetbench/swissmap:swissmap_benchmark"
    "//fleetbench/tcmalloc:empirical_driver"
    "//fleetbench/compression:compression_benchmark"
    "//fleetbench/hashing:hashing_benchmark"
    "//fleetbench/stl:cord_benchmark"
    "//fleetbench/rpc:rpc_benchmark"
  )
  echo "Using ${#benchmark_targets[@]} FleetBench benchmark targets."
  local benchmark_target
  for benchmark_target in "${benchmark_targets[@]}"; do
    local target_relative="${benchmark_target#//}"
    local target_package="${target_relative%%:*}"
    local workload="${target_relative##*:}"
    if ! is_workload_selected "${workload}" "$@"; then
      continue
    fi
    local benchmark_binary
    if [[ -n "${target_package}" ]]; then
      benchmark_binary="${fleetbench_dir}/bazel-bin/${target_package}/${workload}"
    else
      benchmark_binary="${fleetbench_dir}/bazel-bin/${workload}"
    fi
    rm -f /tmp/memprof.profraw.*
    rm -f /tmp/default.profraw
    rm -f "/tmp/${workload}"
    local -a benchmark_build_cmd=(
      "${bazel_cmd}" build
      --compilation_mode=opt
      --custom_malloc=@bazel_tools//tools/cpp:malloc
      --repo_env="CC=${memprof_clang}"
      --repo_env="CXX=${memprof_clangxx}"
      --repo_env=BAZEL_COMPILER=clang
      --action_env="CC=${memprof_clang}"
      --action_env="CXX=${memprof_clangxx}"
      --action_env=BAZEL_COMPILER=clang
      --fission=no
      --strip=never
      --cxxopt=-std=c++17
      --copt=-g
      --copt=-fstandalone-debug
      --copt=-fdebug-info-for-profiling
      --copt=-fmemory-profile=/tmp
      --copt=-fprofile-generate=/tmp
      --copt=-mllvm
      --copt=-memprof-histogram
      --copt=-mno-omit-leaf-frame-pointer
      --copt=-fno-omit-frame-pointer
      --copt=-fno-optimize-sibling-calls
      --copt=-fno-pie
      --linkopt=-g
      --linkopt=-fmemory-profile=/tmp
      --linkopt=-fprofile-generate=/tmp
      --linkopt=-Wl,--build-id
      --linkopt=-no-pie
      "${benchmark_target}"
    )
    echo "Building FleetBench benchmark: ${workload}"
    echo "Target: ${benchmark_target}"
    printf '%q ' "${benchmark_build_cmd[@]}"
    echo
    if ! (
      cd "${fleetbench_dir}" || exit 1
      CC="${memprof_clang}" \
      CXX="${memprof_clangxx}" \
      "${benchmark_build_cmd[@]}"
    ); then
      echo "ERROR: FleetBench build failed for ${workload}." >&2
      echo "========================================="
      continue
    fi
    if [[ ! -x "${benchmark_binary}" ]]; then
      echo "ERROR: FleetBench benchmark binary not found:" >&2
      echo "       ${benchmark_binary}" >&2
      echo "========================================="
      continue
    fi
    cp "${benchmark_binary}" "/tmp/${workload}"
    echo "Running FleetBench benchmark: ${workload}"
    printf '%q ' "${benchmark_binary}"
    echo
    local run_status=0
    (
      cd "${fleetbench_dir}" || exit 1
      "${benchmark_binary}"
    ) || run_status=$?
    if [[ "${run_status}" -ne 0 ]]; then
      echo "WARNING: FleetBench benchmark ${workload} exited with status ${run_status}." >&2
      echo "         Attempting to process any profile it produced." >&2
    fi
    if ! process_workload_memory_profiles \
      "/tmp" "${workload}" "/tmp/${workload}"; then
      echo "ERROR: Failed to process MemProf profile for ${workload}." >&2
      echo "========================================="
      continue
    fi
    echo "Finished FleetBench benchmark: ${workload}"
    echo "========================================="
  done
  echo "Finished processing FleetBench workloads."
}
# ============ Redis ====================
REDIS_VERSION="${REDIS_VERSION:-7.0.7}"
MEMTIER_VERSION="${MEMTIER_VERSION:-1.4.0}"
REDIS_NUM_PROCESSES="${REDIS_NUM_PROCESSES:-20}"
REDIS_TEST_TIME="${REDIS_TEST_TIME:-10}"
REDIS_SERVER_CPUS="${REDIS_SERVER_CPUS:-0-9,20-29}"
REDIS_CLIENT_CPUS="${REDIS_CLIENT_CPUS:-10-19,30-39}"
REDIS_DIR="${TOP_DIR}/third_party/redis-memtier"
redis_workloads=("redis")
build_redis() {
  echo "Building Redis ${REDIS_VERSION} with MemProf..."
  local memprof_clang="${LLVM_BIN_DIR}/clang"
  local memprof_clangxx="${LLVM_BIN_DIR}/clang++"
  local build_root
  local redis_archive
  local memtier_archive
  local redis_source
  local memtier_source
  local output_dir
  local memprof_cflags
  local memprof_ldflags
  if [[ ! -x "${memprof_clang}" ]]; then
    echo "ERROR: MemProf clang not found: ${memprof_clang}" >&2
    return 1
  fi
  if [[ ! -x "${memprof_clangxx}" ]]; then
    echo "ERROR: MemProf clang++ not found: ${memprof_clangxx}" >&2
    return 1
  fi
  for command_name in wget tar make autoreconf; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
      echo "ERROR: Required command not found: ${command_name}" >&2
      return 1
    fi
  done
  build_root=$(mktemp -d "${TOP_DIR}/redis-build.XXXXXX") || return 1
  redis_archive="${build_root}/redis-${REDIS_VERSION}.tar.gz"
  memtier_archive="${build_root}/memtier-${MEMTIER_VERSION}.tar.gz"
  redis_source="${build_root}/redis-${REDIS_VERSION}"
  memtier_source="${build_root}/memtier_benchmark-${MEMTIER_VERSION}"
  output_dir="${build_root}/output"
  mkdir -p "${output_dir}"
  memprof_cflags="-O3 -g -fdebug-info-for-profiling -fno-omit-frame-pointer -fno-optimize-sibling-calls -fmemory-profile=/tmp -fprofile-generate=/tmp -mllvm -memprof-histogram"
  memprof_ldflags="-fmemory-profile=/tmp -fprofile-generate=/tmp -fuse-ld=lld -Wl,--build-id -no-pie"
  echo "Downloading Redis ${REDIS_VERSION}..."
  if ! wget -q --show-progress \
      "https://github.com/redis/redis/archive/refs/tags/${REDIS_VERSION}.tar.gz" \
      -O "${redis_archive}"; then
    rm -rf "${build_root}"
    return 1
  fi
  if ! tar -xzf "${redis_archive}" -C "${build_root}"; then
    rm -rf "${build_root}"
    return 1
  fi
  echo "Redis CC: ${memprof_clang}"
  if ! make -C "${redis_source}" -j"$(nproc)" \
      CC="${memprof_clang}" \
      CXX="${memprof_clangxx}" \
      CFLAGS="${memprof_cflags}" \
      LDFLAGS="${memprof_ldflags}"; then
    rm -rf "${build_root}"
    return 1
  fi
  install -m 755 "${redis_source}/src/redis-server" "${output_dir}/redis-server"
  install -m 755 "${redis_source}/src/redis-benchmark" "${output_dir}/redis-benchmark"
  install -m 755 "${redis_source}/src/redis-cli" "${output_dir}/redis-cli"
  echo "Downloading memtier_benchmark ${MEMTIER_VERSION}..."
  if ! wget -q --show-progress \
      "https://github.com/RedisLabs/memtier_benchmark/archive/refs/tags/${MEMTIER_VERSION}.tar.gz" \
      -O "${memtier_archive}"; then
    rm -rf "${build_root}"
    return 1
  fi
  if ! tar -xzf "${memtier_archive}" -C "${build_root}"; then
    rm -rf "${build_root}"
    return 1
  fi
  echo "Building memtier_benchmark with Clang..."
  if ! (
    cd "${memtier_source}" || exit 1
    autoreconf -ivf
    CC="${memprof_clang}" CXX="${memprof_clangxx}" ./configure --disable-tls
    make -j"$(nproc)"
  ); then
    rm -rf "${build_root}"
    return 1
  fi
  install -m 755 "${memtier_source}/memtier_benchmark" \
    "${output_dir}/memtier_benchmark"
  rm -rf "${REDIS_DIR}"
  mv "${output_dir}" "${REDIS_DIR}"
  rm -rf "${build_root}"
  echo "Redis and memtier_benchmark installed in ${REDIS_DIR}"
}
run_redis() {
  echo "Processing Redis workload..."
  if [[ ! -x "${REDIS_DIR}/redis-server" ]]; then
    echo "ERROR: Redis server not found: ${REDIS_DIR}/redis-server" >&2
    echo "       Run $0 --build-redis first." >&2
    return 1
  fi
  if [[ ! -x "${REDIS_DIR}/redis-cli" ]]; then
    echo "ERROR: redis-cli not found: ${REDIS_DIR}/redis-cli" >&2
    return 1
  fi
  if [[ ! -x "${REDIS_DIR}/memtier_benchmark" ]]; then
    echo "ERROR: memtier_benchmark not found: ${REDIS_DIR}/memtier_benchmark" >&2
    return 1
  fi
  if [[ ! "${REDIS_NUM_PROCESSES}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: REDIS_NUM_PROCESSES must be a positive integer." >&2
    return 1
  fi
  if [[ ! "${REDIS_TEST_TIME}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: REDIS_TEST_TIME must be a positive integer." >&2
    return 1
  fi
  local -a server_pids=()
  local -a server_ports=()
  local -a client_pids=()
  local -a client_ports=()
  local -a server_affinity=()
  local -a client_prefix=()
  local -a benchmark_logs=()
  local i port pid status
  local client_failed=0
  stop_redis_servers() {
    local redis_port redis_pid
    for redis_port in "${server_ports[@]}"; do
      "${REDIS_DIR}/redis-cli" -h ::1 -p "${redis_port}" shutdown nosave \
        >/dev/null 2>&1 || true
    done
    for redis_pid in "${server_pids[@]}"; do
      if kill -0 "${redis_pid}" 2>/dev/null; then
        kill -TERM "${redis_pid}" 2>/dev/null || true
      fi
    done
    for redis_pid in "${server_pids[@]}"; do
      wait "${redis_pid}" 2>/dev/null || true
    done
  }
  if command -v taskset >/dev/null 2>&1 && \
      taskset -c "${REDIS_SERVER_CPUS}" true >/dev/null 2>&1; then
    server_affinity=(--server_cpulist "${REDIS_SERVER_CPUS}")
  else
    echo "WARNING: REDIS_SERVER_CPUS=${REDIS_SERVER_CPUS} is unavailable; Redis will not be pinned." >&2
  fi
  if command -v taskset >/dev/null 2>&1 && \
      taskset -c "${REDIS_CLIENT_CPUS}" true >/dev/null 2>&1; then
    client_prefix=(taskset -c "${REDIS_CLIENT_CPUS}")
  else
    echo "WARNING: REDIS_CLIENT_CPUS=${REDIS_CLIENT_CPUS} is unavailable; clients will not be pinned." >&2
  fi
  rm -f /tmp/memprof.profraw.*
  rm -f /tmp/default.profraw
  cd "${REDIS_DIR}" || return 1
  port=6379
  echo "Starting ${REDIS_NUM_PROCESSES} Redis servers..."
  for ((i = 0; i < REDIS_NUM_PROCESSES; ++i)); do
    ./redis-server \
      --bind ::1 \
      --protected-mode yes \
      --port "${port}" \
      "${server_affinity[@]}" \
      --maxmemory-policy allkeys-lru \
      --io-threads-do-reads no \
      --maxmemory 1024mb \
      >"server_${port}.log" 2>&1 &
    pid=$!
    server_pids+=("${pid}")
    server_ports+=("${port}")
    port=$((port + 1))
  done
  for i in "${!server_pids[@]}"; do
    local ready=false
    local attempt
    for ((attempt = 0; attempt < 100; ++attempt)); do
      if ./redis-cli -h ::1 -p "${server_ports[$i]}" ping 2>/dev/null | grep -q '^PONG$'; then
        ready=true
        break
      fi
      if ! kill -0 "${server_pids[$i]}" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if [[ "${ready}" != true ]]; then
      echo "ERROR: Redis on port ${server_ports[$i]} did not become ready." >&2
      tail -n 50 "server_${server_ports[$i]}.log" >&2 || true
      stop_redis_servers
      return 1
    fi
  done
  port=6379
  echo "Preloading all Redis instances..."
  for ((i = 0; i < REDIS_NUM_PROCESSES; ++i)); do
    echo "Preloading port ${port}..."
    if ! ./memtier_benchmark \
        --server ::1 \
        --port "${port}" \
        --protocol redis \
        --clients 1 \
        --threads 1 \
        --ratio 1:0 \
        --data-size 1024 \
        --pipeline 100 \
        --key-minimum 1 \
        --key-maximum 2800000 \
        --key-pattern P:P \
        --requests allkeys \
        --random-data \
        >"preload_${port}.log" 2>&1; then
      tail -n 50 "preload_${port}.log" >&2 || true
      stop_redis_servers
      return 1
    fi
    port=$((port + 1))
  done
  port=6379
  echo "Running Redis clients for ${REDIS_TEST_TIME} seconds..."
  for ((i = 0; i < REDIS_NUM_PROCESSES; ++i)); do
    "${client_prefix[@]}" ./memtier_benchmark \
      --server ::1 \
      --port "${port}" \
      --protocol redis \
      --clients 8 \
      --threads 4 \
      --ratio 1:9 \
      --data-size 1024 \
      --pipeline 1 \
      --key-minimum 1 \
      --key-maximum 10000000 \
      --key-pattern R:R \
      --run-count 1 \
      --test-time "${REDIS_TEST_TIME}" \
      --print-percentile 50,90,95,99,99.9 \
      --random-data \
      >"benchmark_${port}.log" 2>&1 &
    client_pids+=("$!")
    client_ports+=("${port}")
    benchmark_logs+=("benchmark_${port}.log")
    port=$((port + 1))
  done
  for i in "${!client_pids[@]}"; do
    if wait "${client_pids[$i]}"; then
      :
    else
      status=$?
      echo "ERROR: Redis client for port ${client_ports[$i]} failed with status ${status}." >&2
      tail -n 50 "benchmark_${client_ports[$i]}.log" >&2 || true
      client_failed=1
    fi
  done
  stop_redis_servers
  if ((client_failed != 0)); then
    return 1
  fi
  awk '
    $1 == "Totals" {
      total += $2
      count++
    }
    END {
      if (count == 0) {
        print "ERROR: No Totals rows found in Redis benchmark logs" > "/dev/stderr"
        exit 1
      }
      printf "Total Redis throughput: %.2f ops/sec\n", total
    }
  ' "${benchmark_logs[@]}" || return 1
  process_workload_memory_profiles "/tmp" "redis" "${REDIS_DIR}/redis-server"
  echo "Finished processing Redis workload."
}
# ============ LLAMA ====================
# LLAMA (Low Level Abstraction of Memory Access) examples.
#
# Assumes the checkout already exists at ${TESTSUITE_DIR}/llama.
# Nothing is fetched here; missing dependencies are reported, not installed.
#
# Each example is built as a single translation unit with clang++ directly
# (no CMake, no OpenMP, no xsimd, no alpaka/CUDA). Without -march the AVX2
# code paths are preprocessed out, which keeps the binaries portable for the
# FireSim/RISC-V leg and keeps MemProf callstacks free of OpenMP frames.
#
# Why these four:
#
#   nbody         All-pairs O(N^2) kernel implemented over AoS, SoA (single
#                 blob and one-blob-per-field), AoSoA, Split, Bytesplit and
#                 BitPacked mappings, PLUS hand-written AoS/SoA/AoSoA
#                 baselines. Best ground truth: identical access pattern,
#                 controlled variation in layout and in allocation count
#                 (1 blob vs 7).
#   heatequation  Same mapping menu, but a 2D stencil access pattern instead
#                 of all-pairs. Neighbour access rather than streaming, so it
#                 stresses the same layouts differently.
#   viewcopy      Compares approaches to copying between llama::Views, i.e.
#                 pure allocation-to-allocation traffic at differing strides.
#                 Closest match to what MemProf actually records.
#   vectoradd     Trivial two-array kernel. Sanity baseline.
#
# Not enabled by default (external dependencies): daxpy and babelstream
# (alpaka), the PIC / ROOT-LHCb / CUDA / SYCL examples. Add them to
# llama_workloads if you have the deps and want to try.
LLAMA_SRC_DIR="${LLAMA_SRC_DIR:-${TESTSUITE_DIR}/llama}"
LLAMA_BUILD_DIR="${LLAMA_BUILD_DIR:-${TESTSUITE_DIR}/llama-memprof}"
# Header search. Leave unset to auto-probe the usual locations; set explicitly
# to override.
FMT_INC="${FMT_INC:-}"
BOOST_INC="${BOOST_INC:-}"
# Applied to any example that declares these as top-level constexpr; examples
# that do not are left at their upstream defaults (reported at build time).
#   nbody: 16384 particles * 28 B ~= 448 KB per layout (fits L2/L3).
#   Bump to 262144 (~7 MB) to push the memory-bound phase out of cache, but
#   note the nbody update kernel is O(N^2): 16x particles is 256x the work.
LLAMA_PROBLEM_SIZE="${LLAMA_PROBLEM_SIZE:-16384}"
LLAMA_STEPS="${LLAMA_STEPS:-5}"
# Left empty on purpose: no -march means the AVX2 namespaces are preprocessed
# out, which keeps the binaries portable for the FireSim/RISC-V leg.
LLAMA_MARCH="${LLAMA_MARCH:-}"
# Extra flags appended to every LLAMA compile, e.g. "-fopenmp" or "-DFOO=1".
LLAMA_EXTRA_CXXFLAGS="${LLAMA_EXTRA_CXXFLAGS:-}"
llama_workloads=(
  "nbody"
  "heatequation"
  "viewcopy"
  "vectoradd"
)
# Echo the first candidate directory that contains the given probe header.
function find_include_dir() {
  local probe_header="$1"
  local candidate
  for candidate in "${@:2}"; do
    if [[ -n "${candidate}" && -f "${candidate}/${probe_header}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}
# Echo the single .cpp source for a LLAMA example, preferring <name>/<name>.cpp
# and otherwise taking the first .cpp in the example directory.
function llama_example_source() {
  local example="$1"
  local example_dir="${LLAMA_SRC_DIR}/examples/${example}"
  local candidate
  if [[ ! -d "${example_dir}" ]]; then
    return 1
  fi
  candidate="${example_dir}/${example}.cpp"
  if [[ -f "${candidate}" ]]; then
    printf '%s\n' "${candidate}"
    return 0
  fi
  candidate=$(find "${example_dir}" \
    -maxdepth 1 \
    -type f \
    -name '*.cpp' \
    -print \
    -quit)
  if [[ -z "${candidate}" ]]; then
    return 1
  fi
  printf '%s\n' "${candidate}"
}
# Rewrite a top-level "constexpr auto <name> = ...;" if the example has one.
# Absence is not an error: examples use different knob names.
function llama_patch_constexpr() {
  local file="$1"
  local name="$2"
  local value="$3"
  if ! grep -qE "^constexpr auto ${name} = " "${file}"; then
    echo "  ${name}: not present, leaving upstream default"
    return 0
  fi
  sed -i \
    "s|^constexpr auto ${name} = .*$|constexpr auto ${name} = ${value};|" \
    "${file}"
  if ! grep -q "constexpr auto ${name} = ${value};" "${file}"; then
    echo "ERROR: Failed to patch ${name} in ${file}" >&2
    return 1
  fi
  echo "  ${name} = ${value}"
}
# List which of the candidate examples are actually present in the checkout.
function list_llama() {
  local example
  local source_file
  echo "LLAMA examples under ${LLAMA_SRC_DIR}/examples:"
  if [[ -d "${LLAMA_SRC_DIR}/examples" ]]; then
    find "${LLAMA_SRC_DIR}/examples" -mindepth 1 -maxdepth 1 -type d \
      -printf '  %f\n' | sort
  else
    echo "  (examples directory not found)"
  fi
  echo ""
  echo "Configured workloads:"
  for example in "${llama_workloads[@]}"; do
    if source_file=$(llama_example_source "${example}"); then
      echo "  ${example}: ${source_file}"
    else
      echo "  ${example}: NOT FOUND"
    fi
  done
}
build_llama() {
  echo "Building LLAMA workloads..."
  local memprof_clangxx="${LLVM_BIN_DIR}/clang++"
  local llama_inc
  local fmt_inc
  local boost_inc
  local source_file
  local example_dir
  local patched_file
  local binary_path
  local built_count=0
  local failed_count=0
  local -a compile_cmd
  local -a extra_flags
  if [[ ! -x "${memprof_clangxx}" ]]; then
    echo "ERROR: MemProf clang++ not found: ${memprof_clangxx}" >&2
    echo "       LLVM_BIN_DIR is currently: ${LLVM_BIN_DIR:-<unset>}" >&2
    return 1
  fi
  if [[ ! -d "${LLAMA_SRC_DIR}" ]]; then
    echo "ERROR: LLAMA checkout not found: ${LLAMA_SRC_DIR}" >&2
    echo "       Set LLAMA_SRC_DIR if it lives elsewhere." >&2
    return 1
  fi
  llama_inc="${LLAMA_SRC_DIR}/include"
  if [[ ! -f "${llama_inc}/llama/llama.hpp" ]]; then
    echo "ERROR: llama/llama.hpp not found under ${llama_inc}" >&2
    echo "       Check the layout of the checkout at ${LLAMA_SRC_DIR}." >&2
    return 1
  fi
  if ! fmt_inc=$(find_include_dir "fmt/format.h" \
      "${FMT_INC}" \
      "${LLAMA_SRC_DIR}/thirdparty/fmt/include" \
      "/usr/include" \
      "/usr/local/include"); then
    echo "ERROR: fmt headers not found (looked for fmt/format.h)." >&2
    echo "       Install libfmt-dev, or set FMT_INC to a directory" >&2
    echo "       containing fmt/format.h." >&2
    return 1
  fi
  if ! boost_inc=$(find_include_dir "boost/mp11.hpp" \
      "${BOOST_INC}" \
      "/usr/include" \
      "/usr/local/include"); then
    echo "ERROR: Boost.Mp11 headers not found (looked for boost/mp11.hpp)." >&2
    echo "       Install libboost-dev, or set BOOST_INC to a directory" >&2
    echo "       containing boost/mp11.hpp." >&2
    return 1
  fi
  echo "LLAMA include: ${llama_inc}"
  echo "fmt include:   ${fmt_inc}"
  echo "Boost include: ${boost_inc}"
  if ! mkdir -p "${LLAMA_BUILD_DIR}"; then
    echo "ERROR: Could not create build directory: ${LLAMA_BUILD_DIR}" >&2
    return 1
  fi
  for workload in "${llama_workloads[@]}"; do
    if [[ "${benchmark_provided}" == true ]] &&
       ! is_workload_selected \
         "${workload}" "${selected_benchmarks[@]}"; then
      continue
    fi
    echo "Building workload: ${workload}"
    if ! source_file=$(llama_example_source "${workload}"); then
      echo "WARNING: No source found for LLAMA example '${workload}'" >&2
      echo "         Looked under ${LLAMA_SRC_DIR}/examples/${workload}" >&2
      echo "         Skipping. Run --llama-list to see what is available." >&2
      echo "========================================="
      continue
    fi
    example_dir=$(dirname "${source_file}")
    echo "Source: ${source_file}"
    # Patch a copy so the upstream checkout stays pristine and the knobs
    # remain settable from the environment.
    patched_file="${LLAMA_BUILD_DIR}/${workload}.cpp"
    if ! cp "${source_file}" "${patched_file}"; then
      echo "ERROR: Could not copy ${source_file}" >&2
      failed_count=$((failed_count + 1))
      continue
    fi
    llama_patch_constexpr "${patched_file}" "problemSize" \
      "${LLAMA_PROBLEM_SIZE}" || true
    llama_patch_constexpr "${patched_file}" "steps" \
      "${LLAMA_STEPS}" || true
    binary_path="${LLAMA_BUILD_DIR}/${workload}"
    compile_cmd=(
      "${memprof_clangxx}"
      -std=c++20
      -O3
      -g
      -I "${llama_inc}"
      -I "${fmt_inc}"
      -I "${boost_inc}"
      # The examples include "../common/*.hpp" relative to their own
      # directory. The patched copy lives elsewhere, so put the ORIGINAL
      # example directory on the include path: examples/<name>/../common/X
      # then resolves to examples/common/X.
      -I "${example_dir}"
      -I "${LLAMA_SRC_DIR}/examples"
      -DFMT_HEADER_ONLY=1
      -pthread
      -fdebug-info-for-profiling
      -fno-omit-frame-pointer
      -mno-omit-leaf-frame-pointer
      -fno-optimize-sibling-calls
      -fmemory-profile=/tmp
      -fprofile-generate=/tmp
      -mllvm -memprof-histogram
      -fuse-ld=lld
      -Wl,--build-id
      -no-pie
      -m64
    )
    if [[ -n "${LLAMA_MARCH}" ]]; then
      compile_cmd+=("-march=${LLAMA_MARCH}")
    fi
    if [[ -n "${LLAMA_EXTRA_CXXFLAGS}" ]]; then
      read -ra extra_flags <<< "${LLAMA_EXTRA_CXXFLAGS}"
      compile_cmd+=("${extra_flags[@]}")
    fi
    compile_cmd+=(
      "${patched_file}"
      -o "${binary_path}"
    )
    printf '%q ' "${compile_cmd[@]}"
    echo
    if ! "${compile_cmd[@]}"; then
      echo "ERROR: LLAMA build failed for ${workload}." >&2
      echo "       Continuing with the remaining examples." >&2
      failed_count=$((failed_count + 1))
      echo "========================================="
      continue
    fi
    echo "Built: ${binary_path}"
    built_count=$((built_count + 1))
    echo "========================================="
  done
  echo "LLAMA build summary: ${built_count} built, ${failed_count} failed."
  if (( built_count == 0 )); then
    echo "ERROR: No LLAMA workloads were built." >&2
    return 1
  fi
}
run_llama() {
  echo "Processing LLAMA workloads..."
  local binary_path
  local run_status
  for workload in "${llama_workloads[@]}"; do
    if ! is_workload_selected "${workload}" "$@"; then
      continue
    fi
    binary_path="${LLAMA_BUILD_DIR}/${workload}"
    if [[ ! -x "${binary_path}" ]]; then
      echo "WARNING: LLAMA binary not found: ${binary_path}" >&2
      echo "         Run $0 --build-llama first, or it failed to build." >&2
      continue
    fi
    echo "Processing workload: ${workload}"
    rm -f /tmp/memprof.profraw.*
    rm -f /tmp/default.profraw
    # The examples write .tsv plot files into the cwd; run from the build dir.
    run_status=0
    (
      cd "${LLAMA_BUILD_DIR}" || exit 1
      "${binary_path}"
    ) || run_status=$?
    if [[ "${run_status}" -ne 0 ]]; then
      echo "WARNING: LLAMA ${workload} exited with status ${run_status}." >&2
      echo "         Attempting to process any profile it produced." >&2
    fi
    if ! process_workload_memory_profiles \
      "/tmp" "${workload}" "${binary_path}"; then
      echo "ERROR: Failed to process MemProf profile for ${workload}." >&2
      echo "========================================="
      continue
    fi
  done
  echo "Finished processing LLAMA workloads."
}
# ============ Folly ====================
folly_workloads=()
function run_folly() {
  echo "Processing Folly workloads..."
  if [[ ${#folly_workloads[@]} -eq 0 ]]; then
    echo "No Folly workloads defined."
  fi
  echo "Finished processing Folly workloads."
}
function show_help() {
  local exit_status="${1:-1}"
  echo "Usage: $0 [OPTIONS]"
  echo "Options:"
  echo "  --all                    Run all workload categories."
  echo ""
  echo "  --build-spec             Build and run SPEC CPU 2017 workloads."
  echo "  --spec                   Process SPEC CPU 2017 profiles."
  echo "  --build-spec26           Build and run SPEC CPU 2026 workloads."
  echo "  --spec26                 Process SPEC CPU 2026 profiles."
  echo ""
  echo "  --spec-size <size>       Set both SPEC input sizes: test, train, or ref."
  echo "                            Default: test"
  echo "  --spec17-size <size>     Set only the SPEC CPU 2017 input size."
  echo "  --spec26-size <size>     Set only the SPEC CPU 2026 input size."
  echo "  --dump-unresolved-callstacks"
  echo "                            Additionally dump unresolved callstacks to"
  echo "                            *.unresolved, on top of the normal typetree/"
  echo "                            stats run. Disabled by default."
  echo "  --dump-unresolved-callstacks-only"
  echo "                            Skip the normal typetree/stats run and only"
  echo "                            produce *.unresolved for each workload."
  echo "                            Implies --dump-unresolved-callstacks."
  echo ""
  echo "  --build-clang            Build CLANG workloads."
  echo "  --clang                  Run CLANG workloads."
  echo "  --fleetbench             Run FleetBench workloads."
  echo "  --build-redis            Build Redis with MemProf and memtier_benchmark."
  echo "  --redis                  Run Redis, collect MemProf, and process it."
  echo "  --build-llama            Build the LLAMA n-body workload with MemProf."
  echo "  --llama                  Run LLAMA, collect MemProf, and process it."
  echo "  --llama-list             List LLAMA examples found in the checkout."
  echo "  --folly                  Run Folly workloads."
  echo ""
  echo "  --benchmarks <list>      Run only comma-separated benchmarks."
  echo "                            Requires the corresponding category option."
  echo ""
  echo "  Available benchmarks:"
  if [[ ${#spec_workloads[@]} -gt 0 ]]; then
    echo "    SPEC CPU 2017: ${spec_workloads[*]}"
  fi
  if [[ ${#spec26_workloads[@]} -gt 0 ]]; then
    echo "    SPEC CPU 2026: ${spec26_workloads[*]}"
  fi
  if [[ ${#clang_workloads[@]} -gt 0 ]]; then
    echo "    CLANG: ${clang_workloads[*]}"
  fi
  if [[ ${#fleetbench_workloads[@]} -gt 0 ]]; then
    echo "    FleetBench: ${fleetbench_workloads[*]}"
  fi
  if [[ ${#redis_workloads[@]} -gt 0 ]]; then
    echo "    Redis: ${redis_workloads[*]}"
  fi
  if [[ ${#llama_workloads[@]} -gt 0 ]]; then
    echo "    LLAMA: ${llama_workloads[*]}"
  fi
  if [[ ${#folly_workloads[@]} -gt 0 ]]; then
    echo "    Folly: ${folly_workloads[*]}"
  fi
  echo ""
  echo "LLAMA environment overrides:"
  echo "  LLAMA_SRC_DIR            Default: \${TESTSUITE_DIR}/llama"
  echo "  LLAMA_BUILD_DIR          Default: \${TESTSUITE_DIR}/llama-memprof"
  echo "  LLAMA_PROBLEM_SIZE       Default: 16384"
  echo "  LLAMA_STEPS              Default: 5"
  echo "  LLAMA_MARCH              Default: empty (AVX2 paths compiled out)"
  echo "  LLAMA_EXTRA_CXXFLAGS     Extra flags for every LLAMA compile."
  echo "  FMT_INC / BOOST_INC      Override header auto-detection."
  echo ""
  echo "Examples:"
  echo "  $0 --build-spec --spec-size train"
  echo "  $0 --spec --spec-size ref --benchmarks 505.mcf_r,519.lbm_r"
  echo "  $0 --spec --dump-unresolved-callstacks --benchmarks 505.mcf_r"
  echo "  $0 --spec --dump-unresolved-callstacks-only --benchmarks 505.mcf_r"
  echo "  $0 --build-spec26 --spec26-size ref"
  echo "  $0 --all --spec17-size train --spec26-size ref"
  echo "  $0 --build-llama"
  echo "  $0 --llama"
  echo "  $0 --build-llama --benchmarks nbody,heatequation"
  echo "  LLAMA_PROBLEM_SIZE=262144 LLAMA_STEPS=3 $0 --build-llama"
  echo ""
  echo "  --help, -h               Show this help message."
  exit "${exit_status}"
}
# ====== MAIN ====================================================================
[[ -n "${TOP_DIR:-}" ]] || {
  echo "ERROR: TOP_DIR empty. Are you sure you sourced the environment? (source env.sh)" >&2
  exit 1
}
SPEC17_SIZE="${SPEC17_SIZE:-test}"
SPEC26_SIZE="${SPEC26_SIZE:-test}"
dump_unresolved_callstacks=false
dump_unresolved_only=false
run_all=false
build_spec_flag=false
run_spec_flag=false
build_spec26_flag=false
run_spec26_flag=false
run_clang_flag=false
build_clang_flag=false
run_fleetbench_flag=false
build_redis_flag=false
run_redis_flag=false
build_llama_flag=false
run_llama_flag=false
list_llama_flag=false
run_folly_flag=false
benchmark_provided=false
selected_benchmarks=()
available_benchmarks=(
  "${spec_workloads[@]}"
  "${spec26_workloads[@]}"
  "${clang_workloads[@]}"
  "${fleetbench_workloads[@]}"
  "${redis_workloads[@]}"
  "${llama_workloads[@]}"
  "${folly_workloads[@]}"
)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      run_all=true
      ;;
    --build-spec)
      build_spec_flag=true
      ;;
    --spec)
      run_spec_flag=true
      ;;
    --build-spec26)
      build_spec26_flag=true
      ;;
    --spec26)
      run_spec26_flag=true
      ;;
    --spec-size)
      if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
        echo "ERROR: No size specified for --spec-size." >&2
        show_help 1
      fi
      validate_spec_size "--spec-size" "$2" || exit 1
      SPEC17_SIZE="$2"
      SPEC26_SIZE="$2"
      shift
      ;;
    --spec17-size)
      if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
        echo "ERROR: No size specified for --spec17-size." >&2
        show_help 1
      fi
      validate_spec_size "--spec17-size" "$2" || exit 1
      SPEC17_SIZE="$2"
      shift
      ;;
    --spec26-size)
      if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
        echo "ERROR: No size specified for --spec26-size." >&2
        show_help 1
      fi
      validate_spec_size "--spec26-size" "$2" || exit 1
      SPEC26_SIZE="$2"
      shift
      ;;
    --dump-unresolved-callstacks|--dump_unresolved_callstacks)
      dump_unresolved_callstacks=true
      ;;
    --dump-unresolved-callstacks-only|--dump_unresolved_callstacks_only)
      dump_unresolved_callstacks=true
      dump_unresolved_only=true
      ;;
    --build-clang)
      build_clang_flag=true
      ;;
    --clang)
      run_clang_flag=true
      ;;
    --fleetbench)
      run_fleetbench_flag=true
      ;;
    --build-redis)
      build_redis_flag=true
      ;;
    --redis)
      run_redis_flag=true
      ;;
    --build-llama)
      build_llama_flag=true
      ;;
    --llama)
      run_llama_flag=true
      ;;
    --llama-list)
      list_llama_flag=true
      ;;
    --folly)
      run_folly_flag=true
      ;;
    --benchmarks)
      if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
        echo "ERROR: No benchmarks specified for --benchmarks." >&2
        show_help 1
      fi
      IFS=',' read -ra selected_benchmarks <<< "$2"
      benchmark_provided=true
      shift
      ;;
    --help|-h)
      show_help 0
      ;;
    *)
      echo "ERROR: Unknown option '$1'" >&2
      show_help 1
      ;;
  esac
  shift
done
validate_spec_size "SPEC17_SIZE" "$SPEC17_SIZE" || exit 1
validate_spec_size "SPEC26_SIZE" "$SPEC26_SIZE" || exit 1
if $benchmark_provided; then
  echo "Selecting benchmarks: ${selected_benchmarks[*]}"
else
  echo "No benchmarks selected. Using all available benchmarks."
  selected_benchmarks=("${available_benchmarks[@]}")
fi
echo "Selected benchmarks: ${selected_benchmarks[*]}"
echo "SPEC CPU 2017 input size: ${SPEC17_SIZE}"
echo "SPEC CPU 2026 input size: ${SPEC26_SIZE}"
echo "Dump unresolved callstacks: ${dump_unresolved_callstacks}"
echo "Dump-only unresolved callstacks: ${dump_unresolved_only}"
current_datetime=$(date "+%Y-%m-%d_%H-%M-%S")
curr_experiment="${current_datetime}"
OUT="${TOP_DIR}/out/$curr_experiment"
if [[ "$run_all" == true ||
      "$run_spec_flag" == true ||
      "$run_spec26_flag" == true ||
      "$run_clang_flag" == true ||
      "$run_fleetbench_flag" == true ||
      "$run_redis_flag" == true ||
      "$run_llama_flag" == true ]]; then
  (
    cd "${TOP_DIR}" || exit 1
    bazel build -c opt //src:field_access_tool
  ) || exit 1
fi
base_command="${TOP_DIR}/bazel-bin/src/field_access_tool \
    --stats \
    --local \
    --memprof_profiled_binary"
if [[ "$run_all" == true ]]; then
  echo "Running all workload categories."
  run_spec "${selected_benchmarks[@]}"
  run_spec26 "${selected_benchmarks[@]}"
  run_clang "${selected_benchmarks[@]}"
  run_fleetbench "${selected_benchmarks[@]}"
  run_redis
  run_llama "${selected_benchmarks[@]}"
  run_folly "${selected_benchmarks[@]}"
elif [[ "$run_spec_flag" == true ]]; then
  echo "Running SPEC CPU 2017 workloads with ${SPEC17_SIZE} input."
  run_spec "${selected_benchmarks[@]}"
elif [[ "$build_spec_flag" == true ]]; then
  echo "Building SPEC CPU 2017 and collecting ${SPEC17_SIZE} MemProf profiles."
  build_spec
elif [[ "$run_spec26_flag" == true ]]; then
  echo "Running SPEC CPU 2026 workloads with ${SPEC26_SIZE} input."
  run_spec26 "${selected_benchmarks[@]}"
elif [[ "$build_spec26_flag" == true ]]; then
  echo "Building SPEC CPU 2026 and collecting ${SPEC26_SIZE} MemProf profiles."
  build_spec26
elif [[ "$run_clang_flag" == true ]]; then
  echo "Running CLANG workloads."
  run_clang "${selected_benchmarks[@]}"
elif [[ "$build_clang_flag" == true ]]; then
  echo "Building CLANG."
  build_clang
elif [[ "$run_fleetbench_flag" == true ]]; then
  echo "Running FleetBench workloads."
  run_fleetbench "${selected_benchmarks[@]}"
elif [[ "$build_redis_flag" == true ]]; then
  echo "Building Redis with MemProf and building memtier_benchmark."
  build_redis
elif [[ "$run_redis_flag" == true ]]; then
  echo "Running Redis workload."
  run_redis
elif [[ "$list_llama_flag" == true ]]; then
  list_llama
elif [[ "$build_llama_flag" == true && "$run_llama_flag" == true ]]; then
  echo "Building LLAMA workloads."
  build_llama || exit 1
  echo "Running LLAMA workloads."
  run_llama "${selected_benchmarks[@]}"
elif [[ "$build_llama_flag" == true ]]; then
  echo "Building LLAMA workloads."
  build_llama
elif [[ "$run_llama_flag" == true ]]; then
  echo "Running LLAMA workloads."
  run_llama "${selected_benchmarks[@]}"
elif [[ "$run_folly_flag" == true ]]; then
  echo "Running Folly workloads."
  run_folly "${selected_benchmarks[@]}"
else
  echo "No workloads specified. Use --all or a specific category/benchmark."
  show_help 1
fi
exit 0