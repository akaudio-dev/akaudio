#!/bin/bash
# Static-analysis sweep of the non-vendored sources (src/, minus src/dep/).
# Usage: tools/cppcheck.sh          (from the repo root; exits nonzero on findings)
#
# Intentional deviations are marked in-source with `// cppcheck-suppress <id>`
# comments (picked up via --inline-suppr). Two checks are suppressed wholesale
# as repo style, not code problems:
#   useStlAlgorithm — raw index/range loops are the house idiom here
#   cstyleCast      — C casts on wire/byte-buffer code (TLS, NINJAM protocol)
# missingInclude* is noise: we don't hand cppcheck the Rack SDK include tree.
set -e
cd "$(dirname "$0")/.." >/dev/null

cppcheck \
	--std=c++11 --language=c++ --platform=unix64 \
	--enable=warning,style,performance,portability \
	--check-level=exhaustive \
	--inline-suppr \
	--suppress=missingIncludeSystem --suppress=missingInclude \
	--suppress=useStlAlgorithm --suppress=cstyleCast \
	-DARCH_MAC \
	-I src \
	-i src/dep \
	--error-exitcode=2 \
	--quiet \
	src
echo "cppcheck: clean"
