#!/usr/bin/env bash
# Replay athena75_rgb_advanced commits (fork..tip) onto openkbd/ava. Rewrites feat/rle-mcu-tween.
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"
GIT="${GIT:-git.exe}"

UPSTREAM="${UPSTREAM:-openkbd/ava}"
BRANCH="${BRANCH:-feat/rle-mcu-tween}"
BACKUP_TAG="${BACKUP_TAG:-backup/pre-rebase-feat/rle-mcu-tween}"
TIP="${TIP:-HEAD}"
TIP_SHA="$(${GIT} rev-parse "${TIP}")"
FORK="${FORK:-$(${GIT} log --reverse --format=%H --grep='fork advanced board to athena75_rgb_advanced' -1 "${TIP_SHA}")}"

${GIT} fetch openkbd ava
${GIT} cherry-pick --abort 2>/dev/null || true

${GIT} merge-base --is-ancestor "${FORK}" "${TIP_SHA}" || {
  echo "error: fork ${FORK} not ancestor of ${TIP_SHA}" >&2
  exit 1
}

${GIT} tag -f "${BACKUP_TAG}" "${TIP_SHA}"
echo ">> backup ${BACKUP_TAG} = $( ${GIT} rev-parse --short ${BACKUP_TAG} )"
echo ">> replay $(${GIT} rev-list --count ${FORK}..${TIP_SHA}) commits onto ${UPSTREAM}"

${GIT} checkout -B "${BRANCH}" "${UPSTREAM}"

echo ">> seed $(${GIT} log -1 --oneline ${FORK})"
${GIT} checkout "${FORK}" -- keyboards/ydkb/athena75_rgb_advanced
${GIT} checkout "${UPSTREAM}" -- keyboards/ydkb/athena75_rgb
${GIT} rm -rf --ignore-unmatch \
  keyboards/ydkb/athena75_rgb_advanced/tools/.patch-queue \
  keyboards/ydkb/athena75_rgb_advanced/tools/_count_patches.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/rebase_onto_openkbd.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/host 2>/dev/null || true
if ${GIT} show "${FORK}:.vscode/settings.json" >/dev/null 2>&1; then
  ${GIT} checkout "${FORK}" -- .vscode/settings.json
  ${GIT} add .vscode/settings.json
fi
${GIT} add keyboards/ydkb/athena75_rgb_advanced
${GIT} commit -C "${FORK}"

resolve() {
  local f
  while IFS= read -r f; do
    [[ -z "${f}" ]] && continue
    case "${f}" in
      keyboards/ydkb/athena75_rgb/*) ${GIT} checkout "${UPSTREAM}" -- "${f}" 2>/dev/null || true ;;
      *) ${GIT} checkout --theirs -- "${f}" 2>/dev/null || true ;;
    esac
    ${GIT} add -- "${f}" 2>/dev/null || true
  done < <(${GIT} diff --name-only --diff-filter=U)
}

while IFS= read -r h; do
  [[ "${h}" == "${FORK}" ]] && continue
  echo ">> $(${GIT} log -1 --oneline ${h})"
  if ! ${GIT} cherry-pick -n "${h}"; then
    resolve
    if [[ -n "$(${GIT} diff --name-only --diff-filter=U)" ]]; then
      echo "error: conflict at ${h}" >&2
      exit 1
    fi
  fi
  if ${GIT} diff --cached --quiet && ${GIT} diff --quiet; then
    ${GIT} commit --allow-empty -C "${h}"
  else
    ${GIT} commit -C "${h}"
  fi
done < <(${GIT} rev-list --reverse "${FORK}".."${TIP_SHA}")

echo ">> done: $(${GIT} rev-list --count ${UPSTREAM}..HEAD) commits above ${UPSTREAM}"
${GIT} log --oneline -5
