#!/usr/bin/env bash
# run.sh compares cppnet's HTTP server with others on the TechEmpower Framework Benchmarks' plaintext and JSON tests:
# it builds each server (bench/web/<server>), checks its responses (verify.py), then runs wrk against it as TechEmpower
# does (with 16 pipelined requests per connection for plaintext), and prints a table of the results, which it also
# saves in build/bench-web.
#
#   bench/web/run.sh [server ...]   # cppnet go hyper bun node (all, by default)
#
# The environment can set DURATION and WARMUP (seconds per run, default 10 and 5), THREADS (wrk's, default 4), and
# PLAINTEXT_CONNECTIONS and JSON_CONNECTIONS (lists of how many connections to run each test with).
#
# NOTE: wrk and the server share the machine, unlike TechEmpower's runs, and TechEmpower runs on Linux: the results
#       compare these servers with each other, here, not with TechEmpower's published ones. Each server uses every
#       CPU it can (Bun can only use one, except on Linux).
#
# Needs wrk, curl, python3, cmake (with build/ configured), go, cargo, bun and node.
set -euo pipefail
set -m # each server in its own process group, so that stopping it stops any processes it started.

web=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$web/../.." && pwd)
out=$root/build/bench-web
mkdir -p "$out"

duration=${DURATION:-10}
warmup=${WARMUP:-5}
threads=${THREADS:-4}
read -r -a plaintext_connections <<<"${PLAINTEXT_CONNECTIONS:-256 1024}"
read -r -a json_connections <<<"${JSON_CONNECTIONS:-64 256}"

servers=("$@")
[[ ${#servers[@]} -eq 0 ]] && servers=(cppnet go hyper bun node)

build() {
    case $1 in
    cppnet) cmake --build "$root/build" --config Release --target bench_web >/dev/null ;;
    go) (cd "$web/go" && go build -o "$out/go-server" .) ;;
    hyper) cargo build --quiet --release --manifest-path "$web/hyper/Cargo.toml" --target-dir "$out/hyper" ;;
    bun | node) ;;
    *) echo "unknown server: $1" >&2 && return 1 ;;
    esac
}

version() {
    case $1 in
    cppnet)
        local compiler
        compiler=$(sed -En 's/^set\(CMAKE_CXX_COMPILER_(ID|VERSION) "(.*)"\)$/\2/p' "$root"/build/CMakeFiles/*/CMakeCXXCompiler.cmake | paste -sd' ' -)
        echo "$(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet HEAD || echo +changes), $compiler"
        ;;
    go) go version | cut -d' ' -f3 ;;
    hyper) echo "hyper $(grep -A1 '^name = "hyper"$' "$web/hyper/Cargo.lock" | sed -n 's/version = "\(.*\)"/\1/p'), $(rustc --version | cut -d' ' -f1-2)" ;;
    bun) echo "bun $(bun --version)" ;;
    node) echo "node $(node --version)" ;;
    esac
}

start() {
    case $1 in
    cppnet) exec "$root/build/bench/Release/bench_web" ;;
    go) exec "$out/go-server" ;;
    hyper) exec "$out/hyper/release/bench-web-hyper" ;;
    bun) exec bun "$web/bun/server.ts" ;;
    node) exec node "$web/node/server.js" ;;
    esac
}

# wrk_run test connections port: runs wrk, printing its output.
wrk_run() {
    local test=$1 connections=$2 port=$3 seconds=$4
    local accept='application/json,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7'
    local pipeline=()
    if [[ $test == plaintext ]]; then
        accept='text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7'
        pipeline=(-s "$web/pipeline.lua" -- 16)
    fi

    wrk -H 'Host: localhost' -H "Accept: $accept" -H 'Connection: keep-alive' --latency --timeout 8 \
        -d "$seconds" -c "$connections" -t "$threads" "http://127.0.0.1:$port/$test" "${pipeline[@]}"
}

# summary test: reads wrk's output, printing "requests/s | p50 | p99 | errors". Latencies are left out for plaintext:
# wrk's are meaningless with pipelining.
summary() {
    awk -v pipelined="$([[ $1 == plaintext ]] && echo 1)" '
        /Requests\/sec/ { rps = $2 }
        $1 == "50%" { p50 = $2 }
        $1 == "99%" { p99 = $2 }
        /Socket errors/ { gsub(",", ""); errors += $4 + $6 + $8 + $10 }
        /Non-2xx/ { errors += $5 }
        END {
            if (pipelined) p50 = p99 = "n/a"
            printf "%\047.0f | %s | %s | %d", rps, p50, p99, errors
        }'
}

stamp=$(date +%Y%m%d-%H%M%S)
results=$out/results-$stamp.md
{
    echo "# TechEmpower plaintext and JSON, $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(getconf _NPROCESSORS_ONLN) CPUs, $(uname -sr); wrk -t $threads, ${duration}s per run"
    echo
} >"$results"

rows=()
port=9080
for server in "${servers[@]}"; do
    port=$((port + 1))
    echo "== $server: building"
    if ! build "$server"; then
        echo "== $server: failed to build, skipped"
        rows+=("| $server | failed to build | | | | |")
        continue
    fi
    echo "- $server: $(version "$server")" >>"$results"

    PORT=$port start "$server" >"$out/$server.log" 2>&1 &
    pid=$!

    for _ in $(seq 100); do
        curl -s -o /dev/null "http://127.0.0.1:$port/plaintext" && break
        sleep 0.1
    done

    if ! python3 -I "$web/verify.py" "$port"; then
        echo "== $server: failed verification, skipped"
        rows+=("| $server | failed verification | | | | |")
        kill -TERM -- -"$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        continue
    fi

    for test in plaintext json; do
        if [[ $test == plaintext ]]; then levels=("${plaintext_connections[@]}"); else levels=("${json_connections[@]}"); fi

        echo "== $server: $test, warming up"
        wrk_run "$test" "${levels[0]}" "$port" "$warmup" >/dev/null

        for connections in "${levels[@]}"; do
            echo "== $server: $test, $connections connections"
            wrk_run "$test" "$connections" "$port" "$duration" >"$out/$server-$test-$connections.txt"
            rows+=("| $server | $test | $connections | $(summary "$test" <"$out/$server-$test-$connections.txt") |")
        done
    done

    kill -TERM -- -"$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done

{
    echo
    echo "| server | test | connections | requests/s | p50 | p99 | errors |"
    echo "|---|---|--:|--:|--:|--:|--:|"
    printf '%s\n' "${rows[@]}"
} >>"$results"

echo
cat "$results"
echo
echo "(saved in ${results#"$root"/}, with wrk's output)"
