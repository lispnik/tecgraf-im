#!/usr/bin/env bash
# Rewrite the IM dylibs' install_names and inter-library references to
# absolute paths so they can be dlopen'd from runtimes that lack the
# build-time rpath (e.g. SBCL via CFFI). Idempotent — safe to rerun
# after rebuilding IM.
#
# Run from the IM project root:  ./fix-install-names.sh
#
# Determines the build's lib directory the same way tecmake.mak does
# on macOS (TEC_UNAME = "MacOS<major><minor>"), so it tracks rebuilds
# against newer macOS versions automatically.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "fix-install-names.sh: only meaningful on macOS; nothing to do." >&2
  exit 0
fi

major=$(sw_vers -productVersion | cut -f1 -d.)
minor=$(sw_vers -productVersion | cut -f2 -d.)
TEC_UNAME="${TEC_UNAME:-MacOS${major}${minor}}"
LIB_DIR="$ROOT/lib/$TEC_UNAME"

if [ ! -d "$LIB_DIR" ]; then
  echo "fix-install-names.sh: no $LIB_DIR — build IM first." >&2
  exit 1
fi

LIBS=(libim.dylib libim_process.dylib libim_jp2.dylib libim_fftw3.dylib libim_process_omp.dylib)

for lib in "${LIBS[@]}"; do
  path="$LIB_DIR/$lib"
  [ -f "$path" ] || continue
  # Set the dylib's own install_name (its identity).
  install_name_tool -id "$path" "$path"
  # Rewrite each @rpath dependency to an absolute path.
  for dep in "${LIBS[@]}"; do
    install_name_tool -change "@rpath/$dep" "$LIB_DIR/$dep" "$path" 2>/dev/null || true
  done
done

echo "install_names rewritten to absolute paths in $LIB_DIR"
