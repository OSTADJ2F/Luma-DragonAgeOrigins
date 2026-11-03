#!/bin/sh
# Refuses commits that touch the vendored Luma framework.
#
# The Dragon Age Origins mod lives entirely in Source/Games/Dragon Age Origins, Shaders/Dragon Age Origins and
# Data/Dragon Age Origins. Everything else — Source/Core, Source/Tools, Source/External, Shaders/Includes,
# Shaders/Global, the Luma.*.props/xml files and Scripts/ — is the framework, vendored unmodified from Hlib
# Omelchenko's fork (see Source/Games/Dragon Age Origins/PORT-STATUS.md). Editing it turns this repository into a
# second fork, and every future framework update into a conflict.
#
# Framework updates are legitimate, so they are allowed when the commit message starts with "Framework:".
# Installed as .git/hooks/commit-msg, which receives the message file as $1.
#
# Manual run: Scripts/check-mod-boundary.sh <message file>   (or any file whose first line is the message)

set -eu

framework_paths='^(Source/Core/|Source/Tools/|Source/External/|Shaders/Includes/|Shaders/Global/|Source/Games/Luma\.|Luma\.sln$|Luma\.slnx$|Scripts/)'

staged=$(git diff --cached --name-only --diff-filter=ACDMRTUXB)
[ -n "$staged" ] || exit 0

violations=$(printf '%s\n' "$staged" | grep -E "$framework_paths" || true)
[ -n "$violations" ] || exit 0

message_file="${1:-}"
if [ -z "$message_file" ] || [ ! -f "$message_file" ]; then
   message_file="$(git rev-parse --git-dir)/COMMIT_EDITMSG"
fi
if [ -f "$message_file" ]; then
   first_line=$(head -n 1 "$message_file")
   case "$first_line" in
      Framework:*) exit 0 ;;
   esac
fi

printf '%s\n' "commit-msg: this commit changes vendored framework paths:" >&2
printf '%s\n' "$violations" | sed 's/^/  /' >&2
cat >&2 <<'EOF'

The mod must stay in Source/Games/Dragon Age Origins, Shaders/Dragon Age Origins and Data/Dragon Age Origins:
the framework is vendored unmodified so that updating it never conflicts (see PORT-STATUS.md).

If this really is an upstream framework update, start the commit message with "Framework:".
EOF
exit 1
