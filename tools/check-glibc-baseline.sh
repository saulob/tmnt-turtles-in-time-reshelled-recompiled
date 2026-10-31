#!/usr/bin/env bash
# Fails when an ELF file under the given paths needs a glibc newer than MAX_GLIBC, when a glibc runtime component is
# among them, or when a bundled libstdc++ provides less GLIBCXX/CXXABI than another file needs. Files are recognised by
# their ELF magic, whatever their name or mode. The highest GLIBC, GLIBCXX and CXXABI version of every file is printed.
#
#   tools/check-glibc-baseline.sh [--max VERSION] PATH...    check files and folders (default max: 2.31)
#   tools/check-glibc-baseline.sh --self-test                check the version comparison
set -uo pipefail

MAX_GLIBC=2.31

# Components of glibc itself, which an AppImage must take from the machine it runs on.
GLIBC_RUNTIME='^(libc\.so\.6|libm\.so\.6|libmvec\.so\.1|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libutil\.so\.1|libresolv\.so\.2|libanl\.so\.1|libnsl\.so\.1|libthread_db\.so\.1|libBrokenLocale\.so\.1|libnss_[a-z0-9_]+\.so\.2|ld-linux(-[a-z0-9_-]+)?\.so\.[0-9]+|ld-[0-9.]+\.so)$'

# True when dotted version $1 is newer than $2: numeric per component, a missing component counts as 0.
version_gt() {
  local -a a b
  local i x y
  IFS=. read -ra a <<< "$1"
  IFS=. read -ra b <<< "$2"
  for ((i = 0; i < ${#a[@]} || i < ${#b[@]}; i++)); do
    x=$((10#${a[i]:-0}))
    y=$((10#${b[i]:-0}))
    ((x > y)) && return 0
    ((x < y)) && return 1
  done
  return 1
}

self_test() {
  local failed=0 v
  for v in 2.4 2.14 2.30 2.31; do
    version_gt "$v" 2.31 && { echo "FAIL: $v must not exceed 2.31" >&2; failed=1; }
  done
  for v in 2.31.1 2.32 2.35 2.100 3.0; do
    version_gt "$v" 2.31 || { echo "FAIL: $v must exceed 2.31" >&2; failed=1; }
  done
  version_gt 3.4.32 3.4.9 || { echo "FAIL: 3.4.32 must exceed 3.4.9" >&2; failed=1; }
  version_gt 1.3.13 1.3.9 || { echo "FAIL: 1.3.13 must exceed 1.3.9" >&2; failed=1; }
  [ "$failed" = 0 ] && echo "version comparison: ok"
  return "$failed"
}

is_elf() { [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]; }

# The highest version of one symbol-version family (GLIBC, GLIBCXX or CXXABI) a file names, or nothing.
highest_version() {  # file family
  readelf -V -W "$1" 2>/dev/null | grep -oE "Name: $2_[0-9]+(\.[0-9]+)*" | sed "s/^Name: $2_//" | sort -V | tail -1
}

case "${1:-}" in
  --self-test) self_test; exit ;;
  --max) [ $# -ge 2 ] || { echo "usage: $0 [--max VERSION] PATH..." >&2; exit 2; }; MAX_GLIBC="$2"; shift 2 ;;
esac
[ $# -ge 1 ] || { echo "usage: $0 [--max VERSION] PATH..." >&2; exit 2; }
[[ "$MAX_GLIBC" =~ ^[0-9]+(\.[0-9]+)+$ ]] || { echo "ERROR: invalid glibc version '$MAX_GLIBC'" >&2; exit 2; }
self_test >/dev/null || { self_test >&2; exit 1; }

problems=0
checked=0
need_cxx=""
need_abi=""
have_cxx=""
have_abi=""
bundled_cxx=0
while IFS= read -r -d '' file; do
  [ -L "$file" ] && continue
  base="$(basename "$file")"
  if [[ "$base" =~ $GLIBC_RUNTIME ]]; then
    echo "  glibc runtime is bundled: $file" >&2
    problems=$((problems + 1))
  fi
  is_elf "$file" || continue
  soname="$(readelf -d -W "$file" 2>/dev/null | sed -nE 's/.*\(SONAME\).*\[(.*)\].*/\1/p')"
  if [[ -n "$soname" && "$soname" =~ $GLIBC_RUNTIME ]]; then
    echo "  glibc runtime is bundled (SONAME $soname): $file" >&2
    problems=$((problems + 1))
  fi
  checked=$((checked + 1))
  glibc="$(highest_version "$file" GLIBC)"
  cxx="$(highest_version "$file" GLIBCXX)"
  abi="$(highest_version "$file" CXXABI)"
  printf '  %-14s %-16s %-14s %s\n' "${glibc:+GLIBC_}${glibc:-no glibc}" "${cxx:+GLIBCXX_}${cxx:-}" "${abi:+CXXABI_}${abi:-}" "$file"
  if [ -n "$glibc" ] && version_gt "$glibc" "$MAX_GLIBC"; then
    echo "  $file needs GLIBC_$glibc, above the glibc $MAX_GLIBC baseline" >&2
    problems=$((problems + 1))
  fi
  if [ "$soname" = libstdc++.so.6 ]; then
    bundled_cxx=1
    have_cxx="$cxx"
    have_abi="$abi"
  else
    if [ -n "$cxx" ] && { [ -z "$need_cxx" ] || version_gt "$cxx" "$need_cxx"; }; then need_cxx="$cxx"; fi
    if [ -n "$abi" ] && { [ -z "$need_abi" ] || version_gt "$abi" "$need_abi"; }; then need_abi="$abi"; fi
  fi
done < <(find "$@" -type f -print0 | LC_ALL=C sort -z)

[ "$checked" -gt 0 ] || { echo "ERROR: no ELF files found under: $*" >&2; exit 1; }
echo "  $checked ELF files checked, baseline glibc $MAX_GLIBC"
echo "  highest needed: GLIBCXX_${need_cxx:-none} CXXABI_${need_abi:-none}"
if [ "$bundled_cxx" = 1 ]; then
  if [ -n "$need_cxx" ] && { [ -z "$have_cxx" ] || version_gt "$need_cxx" "$have_cxx"; }; then
    echo "  the bundled libstdc++ provides GLIBCXX_${have_cxx:-none}, below the GLIBCXX_$need_cxx other files need" >&2
    problems=$((problems + 1))
  fi
  if [ -n "$need_abi" ] && { [ -z "$have_abi" ] || version_gt "$need_abi" "$have_abi"; }; then
    echo "  the bundled libstdc++ provides CXXABI_${have_abi:-none}, below the CXXABI_$need_abi other files need" >&2
    problems=$((problems + 1))
  fi
fi
[ "$problems" = 0 ]
