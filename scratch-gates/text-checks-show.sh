#!/bin/sh
# Run every text check and print each one's exit code, so a pass is a
# printed rc=0 and not silence. Pass 1 is clean. Pass 2 plants a banned
# word in an sdk header and must print that check red. Then the plant is
# reverted and the words check runs again.
# Usage (from the scratch checkout): sh text-checks-show.sh <commit> <worktree> <logdir>
S=$(cd "$(dirname "$0")" && pwd)
C=$1; T=$2; L=$3
mkdir -p $L
git worktree remove --force $T 2>/dev/null; git worktree prune
git -C ~/bench/xdp-vlan-gates worktree add -q --detach $T $C || exit 1
cd $T || exit 1
echo "tree=$(git log --oneline -1)"
run_pass() {
  tag=$1; n=0; red=0
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    n=$((n + 1)); sh -c "$line" > $L/$tag-$n.log 2>&1; rc=$?
    [ $rc != 0 ] && red=$((red + 1))
    last=$(grep -v '^[[:space:]]*$' $L/$tag-$n.log | tail -1 | cut -c1-110)
    echo "$tag check=$n rc=$rc :: $line :: $last"
  done < $S/checks-text.txt
  echo "$tag executed=$n red=$red"
}
run_pass clean
F=sdk/rocketnet_xdp_frame.h
echo "// planted: the peer address" >> $F
echo "planted=$(git diff --stat | tail -1)"
run_pass planted
git checkout -q -- $F
echo "reverted=$(git status --short | wc -l)"
python3 tools/check_client_server_words.py > $L/words-after.log 2>&1; echo "words-after-revert rc=$?"
cd / && git -C ~/bench/xdp-vlan-gates worktree remove --force $T
echo TEXT-CHECKS-SHOW-DONE
