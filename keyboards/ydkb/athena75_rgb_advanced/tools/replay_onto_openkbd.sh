#!/usr/bin/env bash
# Replay athena75_rgb_advanced history onto openkbd/ava (57 commits from board split).
# Requires tag backup/pre-rebase-feat/rle-mcu-tween pointing at pre-rewrite tip.
set -euo pipefail
REPO_ROOT=/mnt/f/work/vial-qmk-v6
cd "${REPO_ROOT}"
GIT=git.exe
BACKUP=backup/pre-rebase-feat/rle-mcu-tween
SPLIT_PARENT=dd3ba84069
FIRST=0e64660803a1de6884d91bd7dbb25a424b754b4c

${GIT} fetch openkbd ava
${GIT} cherry-pick --abort 2>/dev/null || true
${GIT} checkout -B feat/rle-mcu-tween openkbd/ava
${GIT} branch -f ava openkbd/ava

echo ">> seed commit (fork advanced board)"
${GIT} checkout "${FIRST}" -- keyboards/ydkb/athena75_rgb_advanced
${GIT} checkout openkbd/ava -- keyboards/ydkb/athena75_rgb
${GIT} rm -rf --ignore-unmatch keyboards/ydkb/athena75_rgb_advanced/tools/.patch-queue \
  keyboards/ydkb/athena75_rgb_advanced/tools/_count_patches.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/rebase_onto_openkbd.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/host 2>/dev/null || true
if ${GIT} show "${FIRST}:.vscode/settings.json" >/dev/null 2>&1; then
  ${GIT} checkout "${FIRST}" -- .vscode/settings.json
fi
${GIT} add keyboards/ydkb/athena75_rgb_advanced .vscode/settings.json 2>/dev/null || \
  ${GIT} add keyboards/ydkb/athena75_rgb_advanced
${GIT} commit -C "${FIRST}"

resolve() {
  local f
  while IFS= read -r f; do
    [[ -z "${f}" ]] && continue
    case "${f}" in
      keyboards/ydkb/athena75_rgb/*) ${GIT} checkout openkbd/ava -- "${f}" 2>/dev/null || true ;;
      *) ${GIT} checkout --theirs -- "${f}" 2>/dev/null || true ;;
    esac
    ${GIT} add -- "${f}" 2>/dev/null || true
  done < <(${GIT} diff --name-only --diff-filter=U)
}

while IFS= read -r h <&3; do
  if [[ "${h}" == "${FIRST}" ]]; then
    continue
  fi
  echo ">> ${h} $(${GIT} log -1 --oneline ${h})"
  if ! ${GIT} cherry-pick -n "${h}"; then
    resolve
    if [[ -n "$(${GIT} diff --name-only --diff-filter=U)" ]]; then
      echo "error: still conflicted at ${h}" >&2
      exit 1
    fi
  fi
  if ${GIT} diff --cached --quiet && ${GIT} diff --quiet; then
    ${GIT} commit --allow-empty -C "${h}"
  else
    ${GIT} commit -C "${h}"
  fi
done 3< <(${GIT} rev-list --reverse "${SPLIT_PARENT}".."${BACKUP}")

echo ">> done"
${GIT} rev-list --left-right --count openkbd/ava...HEAD
${GIT} log --oneline -5
