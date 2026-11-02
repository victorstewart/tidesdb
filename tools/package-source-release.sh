#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${TIDESDB_SOURCE_RELEASE_VERSION:-}"
output_root="${1:-${repo_root}/.run/source-release}"

[[ "${version}" =~ ^[0-9]+(\.[0-9]+)+([.-][A-Za-z0-9]+)*$ ]] || {
  echo "package-source-release: TIDESDB_SOURCE_RELEASE_VERSION is required and must be version-like." >&2
  exit 1
}
for command in git gzip sha256sum tar; do
  command -v "${command}" >/dev/null 2>&1 || {
    echo "package-source-release: missing required command '${command}'." >&2
    exit 1
  }
done

git -C "${repo_root}" diff --quiet --exit-code --
git -C "${repo_root}" diff --cached --quiet --exit-code --
if [[ -n "$(git -C "${repo_root}" status --porcelain=v1 --untracked-files=all)" ]]; then
  echo "package-source-release: source worktree must be clean and fully committed." >&2
  exit 1
fi

mkdir -p "$(dirname "${output_root}")"
output_root="$(cd "$(dirname "${output_root}")" && pwd)/$(basename "${output_root}")"
if [[ -e "${output_root}" ]]; then
  echo "package-source-release: output already exists; choose a new output root." >&2
  exit 1
fi
mkdir -p "${output_root}"

commit="$(git -C "${repo_root}" rev-parse --verify HEAD)"
prefix="tidesdb-${version}/"
archive="${output_root}/tidesdb-${version}.tar.gz"
git -C "${repo_root}" archive --format=tar --prefix="${prefix}" "${commit}" |
  gzip -n > "${archive}"

members="${output_root}/archive-members.txt"
tar -tzf "${archive}" > "${members}"
grep -qx "${prefix}CMakeLists.txt" "${members}" || {
  echo "package-source-release: archive is missing its prefixed root CMakeLists.txt." >&2
  exit 1
}
if grep -Ev "^${prefix}" "${members}" | grep -q .; then
  echo "package-source-release: archive contains a member outside the single release prefix." >&2
  exit 1
fi
if grep -Eq '(^|/)\.git(/|$)|(^|/)\.run(/|$)' "${members}"; then
  echo "package-source-release: archive contains repository or task-local state." >&2
  exit 1
fi

archive_sha256="$(sha256sum "${archive}" | awk '{print $1}')"
cat > "${output_root}/RELEASE-RECEIPT.txt" <<EOF
version=${version}
source_commit=${commit}
archive_sha256=${archive_sha256}
EOF
sha256sum "${archive}" > "${output_root}/SHA256SUMS"
printf 'TidesDB source release candidate: %s\n' "${archive}"
