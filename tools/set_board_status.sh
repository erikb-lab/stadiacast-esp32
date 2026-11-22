#!/usr/bin/env bash
# Simple helper to view or update board test status in README.md and web/index.html.
# Usage:
#   ./tools/set_board_status.sh list
#   ./tools/set_board_status.sh <board-id> <tested|community|untested>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
README="${REPO_DIR}/README.md"
WEBHTML="${REPO_DIR}/web/index.html"

if [[ "${1:-list}" == "list" ]]; then
    echo "Supported board IDs and current status in README.md:"
    grep -E "<!-- board:[a-z0-9-]+ -->" "$README" | sed -E 's/.*\| \*\*\[([^]]+)\].*<!-- board:([a-z0-9-]+) --> ([^|]+) \|.*/  \2  ->  \3  (\1)/'
    echo ""
    echo "To update a board's status:"
    echo "  ./tools/set_board_status.sh <board-id> tested"
    echo "  ./tools/set_board_status.sh <board-id> community"
    echo "  ./tools/set_board_status.sh <board-id> untested"
    exit 0
fi

BOARD_ID="${1:-}"
STATE="${2:-}"

if [[ -z "$BOARD_ID" || -z "$STATE" ]]; then
    echo "Usage: $0 <board-id> <tested|community|untested>"
    exit 1
fi

case "$STATE" in
    tested|working)
        BADGE_MD="✅ Tested & Working"
        BADGE_HTML="✅ Tested &amp; Working"
        ;;
    community)
        BADGE_MD="✅ Community Verified"
        BADGE_HTML="✅ Community Verified"
        ;;
    untested)
        BADGE_MD="🧪 Untested"
        BADGE_HTML="🧪 Untested"
        ;;
    *)
        echo "ERROR: Unknown state '$STATE' (use: tested, community, or untested)"
        exit 1
        ;;
esac

if ! grep -q "<!-- board:${BOARD_ID} -->" "$README"; then
    echo "ERROR: Board ID '${BOARD_ID}' not found in README.md. Run '$0 list' to see valid IDs."
    exit 1
fi

python3 - "$README" "$WEBHTML" "$BOARD_ID" "$BADGE_MD" "$BADGE_HTML" <<'PY'
import re, sys

readme_path, web_path, board_id, badge_md, badge_html = sys.argv[1:]

with open(readme_path, "r", encoding="utf-8") as f:
    txt = f.read()
pat_md = re.compile(rf"(<!-- board:{re.escape(board_id)} --> )([^|]+?)(\s*\|)")
txt_new, n1 = pat_md.subn(rf"\1{badge_md}\3", txt)
with open(readme_path, "w", encoding="utf-8") as f:
    f.write(txt_new)

with open(web_path, "r", encoding="utf-8") as f:
    wtxt = f.read()
pat_html = re.compile(rf"(<!-- board:{re.escape(board_id)} -->)([^<]+?)(</td>)")
wtxt_new, n2 = pat_html.subn(rf"\1{badge_html}\3", wtxt)
with open(web_path, "w", encoding="utf-8") as f:
    f.write(wtxt_new)

print(f"Updated '{board_id}' -> '{badge_md}' (README={n1}, web/index.html={n2})")
PY
