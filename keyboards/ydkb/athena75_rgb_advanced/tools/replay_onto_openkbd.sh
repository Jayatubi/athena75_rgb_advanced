#!/usr/bin/env bash
# Replay feat/rle-mcu-tween onto openkbd/ava: linear history of athena75 work only
# (commits reachable from TIP but not from upstream ava). Rewrites feat/rle-mcu-tween.
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

if ! ${GIT} rev-parse "${TIP_SHA}" >/dev/null 2>&1; then
  echo "error: TIP ${TIP} not found" >&2
  exit 1
fi
if ! ${GIT} rev-parse "${FORK}" >/dev/null 2>&1; then
  echo "error: fork commit not found (set FORK=)" >&2
  exit 1
fi
if ! ${GIT} merge-base --is-ancestor "${FORK}" "${TIP_SHA}"; then
  echo "error: fork is not an ancestor of TIP" >&2
  exit 1
fi

${GIT} tag -f "${BACKUP_TAG}" "${TIP_SHA}"
echo ">> safety tag ${BACKUP_TAG} -> $(${GIT} rev-parse --short ${BACKUP_TAG})"

FIRST="${FORK}"
COUNT="$(${GIT} rev-list --count "${FORK}".."${TIP_SHA}")"
echo ">> replay ${COUNT} athena75 commits onto ${UPSTREAM} (fork ${FIRST})"

${GIT} checkout -B "${BRANCH}" "${UPSTREAM}"
${GIT} branch -f ava "${UPSTREAM}" 2>/dev/null || true

echo ">> seed: $(${GIT} log -1 --oneline ${FIRST})"
${GIT} checkout "${FIRST}" -- keyboards/ydkb/athena75_rgb_advanced
${GIT} checkout "${UPSTREAM}" -- keyboards/ydkb/athena75_rgb
${GIT} rm -rf --ignore-unmatch \
  keyboards/ydkb/athena75_rgb_advanced/tools/.patch-queue \
  keyboards/ydkb/athena75_rgb_advanced/tools/_count_patches.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/rebase_onto_openkbd.sh \
  keyboards/ydkb/athena75_rgb_advanced/tools/host 2>/dev/null || true
if ${GIT} show "${FIRST}:.vscode/settings.json" >/dev/null 2>&1; then
  ${GIT} checkout "${FIRST}" -- .vscode/settings.json
  ${GIT} add .vscode/settings.json
fi
${GIT} add keyboards/ydkb/athena75_rgb_advanced
${GIT} commit -C "${FIRST}"

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
  [[ "${h}" == "${FIRST}" ]] && continue
  echo ">> ${h} $(${GIT} log -1 --oneline ${h})"
  if ! ${GIT} cherry-pick -n "${h}"; then
    resolve
    if [[ -n "$(${GIT} diff --name-only --diff-filter=U)" ]]; then
      echo "error: unresolved conflict at ${h}" >&2
      ${GIT} diff --name-only --diff-filter=U
      exit 1
    fi
  fi
  if ${GIT} diff --cached --quiet && ${GIT} diff --quiet; then
    ${GIT} commit --allow-empty -C "${h}"
  else
    ${GIT} commit -C "${h}"
  fi
done < <(${GIT} rev-list --reverse "${FORK}".."${TIP_SHA}")

echo ">> done: $(${GIT} rev-list --count ${UPSTREAM}..HEAD) commits on ${BRANCH}"
${GIT} branch -u origin/${BRANCH} 2>/dev/null || true
${GIT} log --oneline -3
${GIT} diff --stat "${UPSTREAM}" HEAD -- keyboards/ydkb/athena75_rgb_advanced | tail -3
