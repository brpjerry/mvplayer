# MV Player

## Import rules: the document comes first

`docs/import-rules.html` is the source of truth for how a track gets its video: which
tracks are looked up, the search, what is dropped on sight, listening, sharing, review
verdicts, re-import, requests and pauses, and every threshold and word list. The code in
`src/core` implements that page.

Whenever an import rule is to change (added, removed or altered, down to a threshold or
one word in a list), work in this order. Never change the code before the document.

1. **Propose it in a copy.** Copy `docs/import-rules.html` to
   `docs/import-rules.proposed.html` and make the change there, marked like a diff:
   additions highlighted in green, removals highlighted in red and left in place.
   - Words within a rule: `<ins>` and `<del>`.
   - A whole rule, branch, definition or table row: the class `added` or `removed` on
     its `<li>`, `<div class="def">` or `<tr>`.
   - A rule that is altered shows its old wording removed and its new wording added.
   - Un-hide the banner at the top of the copy (`<p class="proposal" hidden>`), and say
     in it in one line what is proposed.
   - New rules get new ids. Existing ids are never renumbered or reused.
   The report's stylesheet already has these marks; the report itself never carries any.
2. **Wait for approval.** Show the proposed copy and stop there. Nothing in `src/`,
   `qml/`, `ci/` or the README changes for the rule yet.
3. **Once approved, make it the report.** Take the highlights out: delete what was
   marked removed, keep what was marked added without its mark, hide the banner again.
   Write the result to `docs/import-rules.html`, delete the proposed copy, and update the
   "transcribed" date in its head.
4. **Then change the code** to match the report, with its tests (`ci/smoke.sh`).

A rule change asked for in passing ("make it 1:20") still starts at step 1. If the code
and the report are found to disagree, say so and ask which is right before changing
either.
