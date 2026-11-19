#!/bin/sh
# A stand-in for GitHub's gh, for tests/git.sh: canned answers for the pull
# request commands blit runs, each call written to $GH_LOG.
printf '%s\n' "$*" >>"${GH_LOG:-/dev/null}"
case "$1 $2" in
    "pr list")
        printf '%s\n' '[{"number":12,"title":"Louder poem","author":{"login":"bo"},"headRefName":"loud","baseRefName":"main","isDraft":false,"reviewDecision":"REVIEW_REQUIRED"},{"number":9,"title":"Draft idea","author":{"login":"cy"},"headRefName":"idea","baseRefName":"main","isDraft":true,"reviewDecision":""}]' ;;
    "pr checkout")
        git checkout -q -B "pr-$3" ;;
    "pr diff")
        printf '%s\n' 'diff --git a/poem.txt b/poem.txt' 'index 1111111..2222222 100644' '--- a/poem.txt' '+++ b/poem.txt' '@@ -1,3 +1,3 @@' ' one' '-two' '+TWO' ' three' ;;
    "pr review") ;;
    "api -X")
        printf '{}\n' ;;
    "api repos/{owner}/{repo}/pulls/12/comments")
        printf '%s\n' '[{"id":101,"path":"poem.txt","line":2,"body":"Why shout?\nIt is loud.","user":{"login":"ada"}},{"id":102,"in_reply_to_id":101,"path":"poem.txt","line":2,"body":"For effect.","user":{"login":"bo"}},{"id":103,"path":"poem.txt","line":4,"body":"Nice.","user":{"login":"ada"}}]' ;;
    *) echo "fake gh: no answer for: $*" >&2; exit 1 ;;
esac
