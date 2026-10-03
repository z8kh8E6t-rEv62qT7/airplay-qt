#!/usr/bin/env bash
# Sourced by release scripts; diagnostics must never replace the build status.
set -E
release_started=$SECONDS
release_phase_started=$SECONDS
release_current_phase=initialization

release_phase() {
  printf '[release] phase=%s elapsed=%ss\n' "$release_current_phase" "$((SECONDS - release_phase_started))" >&2
  release_current_phase=$1
  release_phase_started=$SECONDS
  printf '[release] starting=%s\n' "$release_current_phase" >&2
}

release_error() {
  local status=$1 source=$2 line=$3 command=$4
  printf '[release] ERROR phase=%s status=%s at=%s:%s command=%s\n' "$release_current_phase" "$status" "$source" "$line" "$command" >&2
  return "$status"
}

release_finished() {
  local status=$1
  set +x
  printf '[release] finished phase=%s phase_elapsed=%ss total_elapsed=%ss status=%s\n' "$release_current_phase" "$((SECONDS - release_phase_started))" "$((SECONDS - release_started))" "$status" >&2
  exit "$status"
}

trap 'release_error "$?" "${BASH_SOURCE[0]}" "$LINENO" "$BASH_COMMAND"' ERR
trap 'release_finished "$?"' EXIT
# Bash 3.2 (macOS) supports these fields; do not dump the full environment.
PS4='+ [${SECONDS}s ${BASH_SOURCE[0]}:${LINENO} ${FUNCNAME[0]:-main}] '
set -x
