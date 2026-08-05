#!/bin/bash
# clang-tidy sweep of the non-vendored sources (src/, minus src/dep/), using the
# check set in .clang-tidy. Deeper than tools/cppcheck.sh (path-sensitive Clang
# Static Analyzer, CERT, concurrency checks) but needs a compilation database.
#
# Usage: tools/clang_tidy.sh        (from anywhere; exits nonzero on findings)
#
# Requires Homebrew llvm + bear:  brew install llvm bear
# Regenerates compile_commands.json (make clean + instrumented rebuild) if missing
# or older than the Makefile.
set -e
cd "$(dirname "$0")/.." >/dev/null

LLVM=/opt/homebrew/opt/llvm/bin
[ -x "$LLVM/clang-tidy" ] || { echo "clang-tidy not found — brew install llvm" >&2; exit 1; }

if [ ! -f compile_commands.json ] || [ Makefile -nt compile_commands.json ]; then
	command -v bear >/dev/null || { echo "bear not found — brew install bear" >&2; exit 1; }
	echo "regenerating compile_commands.json (clean instrumented build)..."
	make clean >/dev/null
	bear -- make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
fi

# Homebrew clang-tidy doesn't know the Apple SDK path implicitly like Apple c++.
SYSROOT=$(xcrun --show-sdk-path 2>/dev/null || true)

"$LLVM/run-clang-tidy" -p . -quiet \
	-clang-tidy-binary "$LLVM/clang-tidy" \
	-j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
	${SYSROOT:+-extra-arg=-isysroot$SYSROOT} \
	$(find src -name '*.cpp' -not -path 'src/dep/*') \
	2>/dev/null | grep -v '^$' \
	| grep -v '\.sdk/usr/include/' \
	| { ! grep -E "warning:|error:"; } \
	&& echo "clang-tidy: clean"
# The .sdk filter drops diagnostics whose primary location is inside an Apple SDK
# header — concretely the security.ArrayBound taint FP in Darwin's inline
# FD_SET/FD_ISSET helpers: our fds are bounds-checked against FD_SETSIZE at both
# insert and use (Socket.cpp), but the analyzer's solver can't derive
# fd/NFDBITS < nwords from fd < FD_SETSIZE, and a NOLINT can't target SDK code.
