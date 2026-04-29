# Auto-Improvement Loop for Easel

A design spec for a scheduled background subsystem that, while the user is asleep
or shipping other work, surveys the field of generative shaders / projection art /
real-time graphics, critiques Easel's existing 77-shader library, and opens
narrowly-scoped pull requests that either add INPUTS to existing shaders or
introduce a new `.fs` artifact in the manner of `cubism_braque.fs`. Format mirrors
`art_movement_shaders.md` and `state_of_realtime_2026.md`: assertive, opinionated,
willing to recommend a single path. Audience: the same senior C++/GLSL implementer
who reads those notes — me-in-six-months. Date: April 2026.

---

## 1. Vision & motivation

Easel is shipping ~3 hand-crafted shaders per session. That cadence is excellent
when the user is at the keyboard, but it's the wrong shape for a library that
ought to *compound*. The art-movement series alone (entries 474–476 in
`manifest.json`) implies fifteen more specs queued in `art_movement_shaders.md`,
plus the latent bench of techniques the user hasn't gotten to: bitangent curl
noise, Gray-Scott reaction-diffusion, AGX tonemapping, screen-space curvature,
3DGS backdrops. The bottleneck is not ideas. It is *shipping while doing
something else*.

The auto-improvement loop is the answer. It is a scheduled background job that
runs without supervision, researches what's new, compares it against the
existing 77 entries, and either proposes a parameter delta on an existing
shader or drafts a whole new one. Most cycles end in "no change recommended"
and that is the goal — a critic that ships nothing 80% of the time but ships
the right thing 20% of the time is a far more valuable collaborator than one
that ships every day.

The framing I want to commit to: a **self-curating shader library that absorbs
the field's current vocabulary every day**. Not autonomous merging. Not "AI
makes the art." A continuously running junior collaborator who reads ShaderToy
front-page so I don't have to, drafts a PR titled
`auto-loop: 2026-05-12 — Suprematism (Malevich)`, attaches a critique log,
and waits patiently for me to merge it on the train.

This file specifies the loop's architecture, corpus design, pipeline phases,
guardrails, and the small list of things I couldn't decide alone.

---

## 2. Architecture

### 2.1 Where the job runs

There are four reasonable hosts. They are not equivalent. I'm recommending one.

**Option A — `launchd` agent on the user's Mac.** A `~/Library/LaunchAgents/com.easel.autoloop.plist`
`StartCalendarInterval` entry that fires `claude` (the CLI) with a fixed prompt
and the working directory pinned to `/Users/lu/easel`. Pros: zero infra, runs
when the laptop is awake, no API key juggling, free. Cons: laptop must be on;
no notification surface beyond a log file; if the loop wedges, nothing notices.
Apple's [launchd plist reference](https://www.launchd.info/) and the
[`launchctl` man page](https://ss64.com/mac/launchctl.html) are the only docs
needed.

**Option B — GitHub Actions cron.** A `.github/workflows/auto-loop.yml` with a
`schedule: cron: "0 9 * * *"` trigger (GHA cron is UTC, so 09:00 UTC ≈ 02:00
PT in summer — close enough to 4am). Pros: runs whether the laptop is on or
not, free for public repos, log surface and notification surface built in,
PR creation is a one-liner with `gh pr create`. Cons: requires an
`ANTHROPIC_API_KEY` secret on the repo, runner has no GPU so we can't
*render-test* a generated shader, GHA cron drift can be ±15 min on shared
runners ([GHA scheduled-events docs](https://docs.github.com/en/actions/writing-workflows/choosing-when-your-workflow-runs/events-that-trigger-workflows#schedule)).

**Option C — Claude Agent SDK with a scheduled run.** The Agent SDK
([Anthropic Agent SDK docs](https://docs.anthropic.com/en/docs/agent-sdk/overview))
plus `claude --schedule` (the new routines surface available via the
`/schedule` skill in this very harness) gives us a managed remote agent
running on Anthropic infra. Pros: doesn't need our compute at all, has
first-class tool-use and MCP-server access, naturally integrates with
`claude-mem` and other MCP tools we already have. Cons: cost-per-run is real
(opus tokens add up at daily cadence), no local filesystem access by
default — the agent has to clone the repo each run, and if we're rendering
shaders for visual diff, we still need a runner with a GPU. The harness's
`/schedule` skill does explicitly support routines on a cron schedule and
should be considered the canonical "remote scheduled agent" surface.

**Option D — Local cron via the Claude Agent SDK in headless mode.** Same SDK
as Option C but invoked from a `launchd` plist on the laptop, using the local
`claude` binary with `--print --permission-mode acceptEdits` against a
worktree. Pros: combines local compute (free shader rendering, the user's
filesystem, the user's MCP servers including claude-mem) with the SDK's
tool-driven harness. Cons: same laptop-must-be-on caveat as A; slightly more
plumbing.

**Recommendation: Option D for v1, with an obvious migration path to C.**
The user's laptop is on most working hours; the user already has claude-mem,
`gh`, and a checkout. v1 is a launchd plist that runs `claude --print` with a
single prompt that drives the full pipeline. When the loop has earned
trust — say, after 30 days and a non-zero merged-PR count — graduate to
Option C so it runs even when the laptop is closed. Option B is a fine
escape hatch if Anthropic SDK pricing turns out to be the wrong shape; the
shape of the work is identical.

Reject Option A pure-shell because we want tool use (the loop must read
`manifest.json`, query claude-mem, write files) and an LLM is the right
substrate for "decide if these are duplicates."

### 2.2 Trigger cadence

Daily at 04:00 local. Justification: the field genuinely produces something
worth looking at most days (ShaderToy front page rotates, arXiv has a daily
graphics drop), and the loop's *cost* is bounded — we'll build in a hard
"emit zero PRs" output for cycles where nothing's worth shipping. Weekly
runs let the field move past us and bunch the work into a Monday backlog.
Sub-daily runs spam the PR queue. Daily-at-night means the user wakes up to
either nothing (good) or one PR with a one-paragraph summary (also good).

Pick **04:00 local** specifically because the user is in PT, and 04:00 PT
is mid-morning UTC — fresh content has already been posted to
ShaderToy/arXiv/Two Minute Papers in the previous 8h window.

### 2.3 Worker model: fan-out, not single-agent

A single agent doing research-then-critique-then-write tends to overcommit.
It will research with the writing already in mind and rationalise toward a
PR even when the corpus says "we already have this." Splitting the work
across **three subagents** (researcher, critic, writer), each with its own
prompt and tool budget, recovers the adversarial property we want.

Concretely:

- **researcher** — broad context, web access, arXiv MCP, ShaderToy scraping.
  Does *not* see `manifest.json`. Outputs a candidate slate of 3–7 items
  with one-line summaries and source URLs into the corpus.
- **critic** — narrow context, no web access, given (a) the candidate slate,
  (b) `manifest.json`, (c) corpus search results for each candidate. Decides
  reject / accept-as-parameter-add / accept-as-new-shader. Outputs a written
  critique log per candidate. Picks 0–2 winners.
- **writer** — narrow context, no web access, given the winners and the
  shader-spec voice from `art_movement_shaders.md`. Drafts INPUTS / `.fs` /
  `manifest.json` deltas, opens the PR via `gh pr create`.

The Claude Agent SDK supports subagents directly (`Task` tool /
[Agent SDK subagent docs](https://docs.anthropic.com/en/docs/agent-sdk/subagents)).
For Option D each subagent is a `claude --print` invocation with a separate
system prompt; outputs are passed via temp files. For Option C they're
genuine SDK subagents.

### 2.4 Sandbox & isolation

Each cycle runs in a **dedicated git worktree**, never the user's primary
checkout:

```
/Users/lu/easel-autoloop/worktrees/2026-05-12/
```

The worktree is created from `main`, the loop branches to
`auto-loop/2026-05-12-<slug>`, makes commits, pushes, opens the PR, and the
worktree is deleted on success. The `EnterWorktree` / `ExitWorktree`
harness tools are exactly the right primitive for this and should be used
when the loop runs inside a Claude harness; otherwise plain `git worktree
add` / `git worktree remove` from a shell.

**No auto-merge, ever.** The PR sits in `gh pr list` until the user reviews
it. This is non-negotiable; see §5.

---

## 3. Corpus structure

The corpus is the loop's growing memory of *what it has already considered*.
Without it, every cycle re-discovers Suprematism, re-rejects it for the same
reason, and the user gets a duplicate PR every Tuesday. With it, the loop
queries "have I already seen Malevich's *Black Square*?" before research
even begins, and skips ahead to something it hasn't seen.

The corpus must hold three kinds of entries:

1. **Movement / technique entries** — one per topic the loop has researched.
   Includes URLs consulted, the loop's verdict, and a list of `manifest.json`
   IDs already covering that topic.
2. **Candidate entries** — proposed shaders / parameters that haven't been
   actioned. The critic uses these to avoid re-proposing rejected ideas.
3. **Critique notes** — free-text reasoning about why a candidate was
   rejected, so future cycles inherit the lesson.

### 3.1 Storage: claude-mem as host (recommended primary)

The user already has the
[`claude-mem` MCP server](https://github.com/thedotmack/claude-mem) installed
(plugin `claude-mem-thedotmack`, version 12.2.2). Its core abstraction is the
**observation** — a typed record (decision / bugfix / feature / discovery /
change) with title, subtitle, narrative, facts, concepts, files, plus full
free-text content — searchable by semantic query, by concept tag, by file
prefix, by date range. It's the same substrate the user already uses for
session memory, which means we get free integration with the rest of their
workflow: anything the loop learns is queryable from any future Claude
session in the project.

**Mapping the loop's three entry kinds onto claude-mem's vocabulary:**

| Loop entry           | claude-mem `obs_type` | Concepts                          |
|----------------------|-----------------------|-----------------------------------|
| Movement / technique | `discovery`           | `auto-loop`, `corpus-entry`, `<topic-slug>` |
| Candidate            | `feature`             | `auto-loop`, `candidate`, `<topic-slug>`    |
| Critique             | `decision`            | `auto-loop`, `critique`, `<topic-slug>`     |

Every observation gets the `auto-loop` concept tag so we can scope queries
without polluting the user's regular session memory. Project name in
claude-mem is `easel`.

**A representative observation for "Suprematism / Malevich":**

```json
{
  "obs_type": "discovery",
  "title": "Considered Suprematism (Malevich) for shader library",
  "subtitle": "Already covered by DESTIJL_MONDRIAN + MINIMALISM_STELLA — rejected duplicate",
  "narrative": "Researched 2026-05-12. Source: Tate Modern Suprematism overview + Malevich, Black Square (1915). Vocabulary: pure geometric forms (square, circle, cross), limited palette (black, red, white, ochre), sense of weightless float against unrendered ground. Already covered: MINIMALISM_STELLA shape repertoire (id 461) handles the geometric-cross idiom; DESTIJL_MONDRIAN (id ...) handles the orthogonal palette-block idiom. Distinct contribution would be the *floating, off-axis composition* — not yet in library. Re-evaluate as candidate when CONSTRUCTIVISM_LISSITZKY ships, since they share visual DNA; might absorb into that PR.",
  "facts": [
    "Suprematism dates 1913–1935",
    "Malevich Black Square 1915",
    "Closest manifest entry: 461 MINIMALISM_STELLA"
  ],
  "concepts": ["auto-loop", "corpus-entry", "suprematism", "malevich", "art-movement", "rejected-duplicate"],
  "files": ["external/ShaderClaw3/shaders/manifest.json", ".planning/notes/art_movement_shaders.md"]
}
```

### 3.2 Querying the corpus

Before research, the researcher subagent issues a single
`mcp__plugin_claude-mem_mcp-search__search` call:

```
search(query="suprematism malevich", project="easel", obs_type="discovery", limit=10)
```

Results come back as a token-cheap index (~75 tokens/result) of titles +
IDs. Researcher fetches details on relevant matches via `get_observations`
(batched) and uses them to *avoid* re-proposing topics already seen.

For dense thematic queries the loop occasionally builds a **knowledge-agent
corpus** via `build_corpus` + `prime_corpus` + `query_corpus`. Use that
sparingly — the critic, not the researcher, would benefit from
"summarise everything we've ever rejected as duplicate" when scoring borderline
candidates.

### 3.3 Limits of claude-mem as a knowledge base

Claude-mem is built primarily as a *session memory* tool — its sweet spot is
"what did I do last week?", not "what is my durable knowledge base?" In
practice this manifests as three real limits:

1. **No first-class structured fields.** All structured data has to live in
   the unstructured `narrative` plus the `facts[]` array. No native foreign
   keys to `manifest.json` IDs, no enum field for verdict. We can stash
   structure as JSON-in-narrative and parse on read, but it's not free.
2. **Search is good but not great for structured filtering.** Concept tags
   give us coarse filtering; for "find all corpus entries with verdict=
   rejected-duplicate where the topic-slug starts with `art_`", we'd need
   to fetch a wider candidate set and filter client-side.
3. **Observations are append-only by convention.** Updating an entry means
   creating a new observation that supersedes the old one — fine for an
   audit trail, awkward for "the loop's current view of Suprematism."

None of these are blockers. They cost ~10% of the loop's reasoning budget on
parsing structure-from-text and de-duping superseded entries. Worth it for
the integration value.

### 3.4 Fallback: flat-file corpus

If, after a month, claude-mem proves unsuitable (likely failure mode: the
user's regular session memory fills with auto-loop noise and starts
polluting unrelated queries), fall back to a flat-file corpus:

```
.planning/notes/auto_loop/
  corpus/
    art-movement/
      suprematism-malevich.md
      bauhaus-itten.md
    technique/
      bitangent-noise.md
      gray-scott-rd.md
    rejected/
      <topic-slug>.md
  index.sqlite        # FTS5 over corpus/**/*.md
  meta.json           # {topic_slug → {last_seen, verdict, manifest_ids}}
```

One markdown file per topic, frontmatter for structured fields, SQLite FTS5
for query. Roughly 200 lines of Python to maintain. The loop knows how to
write to either backend — the choice lives in
`.planning/notes/auto_loop/config.toml` as `corpus.backend = "claude-mem"`
or `"flat-file"`.

Default to claude-mem. Only flip if the noise problem is real.

---

## 4. The pipeline (daily)

Three phases, each its own subagent, each with its own prompt and a shared
working directory `/tmp/auto-loop/<date>/`. Inputs and outputs are markdown
files passed by path between phases — coarse but legible, and easy for the
user to inspect post-hoc.

### 4.1 Phase 1 — Research

**Goal:** produce a candidate slate of 3–7 items with one-line summaries
and source URLs, written to `/tmp/auto-loop/<date>/candidates.md`.

**Sources, in priority order:**

1. **ShaderToy front page** (https://www.shadertoy.com/) — fetch the day's
   trending generators. Discount any that are obviously too expensive
   (>120 lines of raymarching) or too narrow (e.g. text effects). Keep 1–2.
2. **Art history rotation.** Maintain a deterministic alphabetical pointer
   in the corpus: `corpus.cursors.art_movement = "F"` after Fauvism is
   covered. Each cycle advances one letter; the researcher proposes the
   *next* movement by alphabet that the corpus hasn't yet seen. Same logic
   for techniques (`A` → "AGX tonemapping", `B` → "bitangent noise",
   `C` → "curvature shading", …). Rotation guarantees coverage across
   ~26-day cycles and avoids the LLM's tendency to over-propose recent
   movements.
3. **arXiv `cs.GR` daily digest** (or its [RSS feed](https://arxiv.org/list/cs.GR/recent))
   — keep one paper per cycle if any look real-time-applicable.
4. **Two Minute Papers** YouTube digest — only as a sanity check / hot-take
   feed; don't take its enthusiasm at face value, but it's a useful
   "is this trending?" signal.

The researcher writes each candidate as:

```markdown
## Candidate: SUPREMATISM_MALEVICH
- Source: Tate Modern (https://...), Malevich Black Square (1915)
- One-line: Floating off-axis pure geometry — black, red, white, ochre
- Why-now: Closes a gap in the Art Movement series after FAUVISM/CUBISM
- Estimated cost: small new shader, ~80 lines GLSL
```

The researcher *does not* see `manifest.json`. That's the critic's job.

### 4.2 Phase 2 — Critique

**Goal:** for each candidate, decide reject / parameter-add / new-shader,
and write a one-paragraph rationale into
`/tmp/auto-loop/<date>/critique.md`. Pick 0–2 winners.

**Inputs:** `candidates.md`, `manifest.json`, corpus search results for
each candidate's topic-slug.

**Decision rubric, in order:**

1. **Already in library?** If `manifest.json` has an entry whose vibe
   overlaps ≥80% with the candidate, *reject as duplicate*. Cite the
   conflicting ID. Example: a "stained glass" candidate is rejected
   because id 473 STAINED_GLASS_CHARTRES exists.
2. **In corpus as already-rejected?** If the corpus already says we
   considered and passed on this, *reject with a "see prior verdict" link*.
   Don't re-litigate.
3. **Cheap parameter add?** If the candidate's distinctive feature could
   be a 1–3 INPUT add to an existing shader (e.g. "a `paletteShift` knob
   on FAUVISM_MATISSE that snaps to the secondary Derain palette") —
   *route to parameter-add path*.
4. **Justifies new shader?** If the candidate has a vibe that
   `manifest.json` simply doesn't cover — *route to new-shader path*.
5. **Otherwise, low-yield reject.** This is the dominant outcome and it is
   correct.

Hard rule: critic picks at most **two** winners. Tie-break by which one
costs less to ship (parameter-add < new-shader). If the critic picks zero,
the cycle ends here with a "no change recommended" PR-comment-equivalent
written to a daily-digest file (see §5.4) and no PR is opened.

The critique log lives in the PR body. Future cycles can search it via
the corpus.

### 4.3 Phase 3 — Parameter-add or new-shader PR

For each winner, the writer subagent picks one of two paths.

**Path A — parameter-add.** Modify the target `.fs` file's INPUTS block,
add the corresponding GLSL plumbing, update `manifest.json` if the
description should change, and commit. Keep the diff under ~80 lines —
larger and the critic should have routed to a new shader instead.

**Path B — new shader.** Draft a complete `.fs` file following the
`art_movement_shaders.md` template:

- vibe paragraph (1 sentence) at the top of the file as a comment
- INPUTS block (max 8, including `inputTex`)
- `vec3 hash(vec2)` / palette helpers if needed
- audio uniforms wired to vibe-specific roles
- main() under 80 lines

Append the new entry to `manifest.json` with the next available id
(highest current is 476; the loop should pick `max(id)+1`). Categories
must include at least one of `Generator`, `Effect`, `Audio Reactive`,
`Art Movement`, `Geometric`, `Nature`, `Ambient` — match the existing
patterns.

**PR construction.**

```
Title: auto-loop: 2026-05-12 — Suprematism (Malevich)

Body:
## Verdict
new-shader (1 of 1 winner this cycle)

## Why
[Critic's rationale, 1 paragraph]

## What changed
- Added external/ShaderClaw3/shaders/suprematism_malevich.fs (78 lines GLSL)
- Added entry id 477 to external/ShaderClaw3/shaders/manifest.json

## Critique log (from this cycle)
[Per-candidate verdicts, including rejections]

## Corpus links
- claude-mem observation #11842 (this entry)
- Prior related: #11201 (CUBISM_BRAQUE corpus entry)

## Test plan
- [ ] Visually inspect at 1080p
- [ ] Check audio reactivity at silence (idle floor) and at full mix
- [ ] Confirm no crash on load (manifest parse)

🤖 Generated by auto-improvement loop
```

Open via `gh pr create` against `main`. **Never auto-merge.**

---

## 5. Trust & guardrails

A loop that ships is one the user has to actively trust. The defaults must
err on the side of doing nothing.

### 5.1 Never auto-merges

Hard rule. The loop has no merge permission. PRs accumulate in `gh pr list`
until the user merges them by hand. If the user wants to delegate merging
later — say to a CI that runs visual diff against a golden frame — that
is a *separate* phase, not part of this loop.

### 5.2 PR rate limit

Hard cap of **3 PRs per 7-day rolling window**. The loop checks
`gh pr list --author "@me" --label auto-loop --state open` before opening
a new PR; if the count is ≥ 3, the cycle ends with no PR even if the
critic picked winners. Open PRs accumulating means the user hasn't
processed them yet — the right response is to back off.

3-per-week is a deliberate floor. The user is shipping ~3 hand-crafted
shaders per session, so 3 from the loop on top of that is at the edge of
"too many to review." If the rate proves wrong, the floor is the first
knob to tune.

### 5.3 Kill switch

A file the user can `touch` to pause the loop. Convention:
`.planning/notes/auto_loop/PAUSE` — the loop checks for this file before
any other action and exits with a "paused" log line if present.

`rm .planning/notes/auto_loop/PAUSE` resumes. The loop never deletes this
file.

There is also a hard **error budget**: three consecutive cycle failures
(cron exit code ≠ 0) auto-create the PAUSE file with the failure log
appended. The user must remove it manually.

### 5.4 Daily digest, scannable in 5 seconds

Every cycle, regardless of outcome, appends a line to
`.planning/notes/auto_loop/digest.md`:

```
2026-05-12 04:02 — researched 5 (Suprematism, AGX, curvature, RD, splat-
  audio) → critic rejected 4 (3 dup, 1 low-yield) → 1 PR opened: #142
  Suprematism (Malevich)
2026-05-13 04:01 — researched 4 → critic rejected 4 (no winners) → 0 PRs
2026-05-14 04:02 — researched 6 → 1 winner, but PR cap reached (3 open) →
  parked candidate Bauhaus-Itten in corpus, 0 PRs
2026-05-15 04:02 — paused (PAUSE file present)
```

The dominant line shape is "0 PRs" and that is a feature. The user reads
the digest in five seconds while the kettle boils.

### 5.5 Cost ceiling

A per-cycle token budget — enforced by the harness (`Agent SDK` budget
APIs) or by exiting early if the researcher exceeds N tool calls. Initial
budget: 50k input + 10k output tokens per cycle, hard ceiling 100k input.
At Opus-class pricing, that's roughly $1–2 per cycle, ~$30–60/mo, which
is the right order of magnitude for "background contributor."

If the loop ever costs more than $100 in a calendar month, the loop
auto-pauses and writes `BUDGET-EXCEEDED` to the digest. The user can lift
the cap manually.

### 5.6 Visual sanity check (deferred)

A nice-to-have, not v1: render the new `.fs` to a single 1280×720 PNG via
the `easel-render` utility (a CLI we don't have yet — TODO) and attach
the PNG to the PR. This is the difference between "the user reviews the
diff" and "the user *sees* the diff." Phase it in once the loop has
shipped 5 merged PRs.

---

## 6. Open questions

Things I couldn't resolve from the codebase alone, listed in
declining priority:

1. **PR target — `main` or an `auto-loop/` integration branch?**
   I've assumed `main`. If the user prefers a parking branch that gets
   batch-merged on Sundays (less merge-conflict surface, easier to abandon
   a week's output if it's all wrong), say so and the writer phase
   re-targets trivially.

2. **Notification surface — Slack? Email? Just the digest?**
   The user has `mcp__claude_ai_Gmail__create_draft` available and a
   Hermes channel framework. Cheapest viable option is "do nothing, the
   PR notification from GitHub is enough." Fanciest is a daily Hermes
   `etherea-intel`-style entry. I lean toward the cheap option for v1
   and bolting on Hermes later if the digest proves invisible.

3. **Should the loop critique its own past PRs?**
   If a PR sits unmerged for 14 days, that is information. Two
   reasonable responses: (a) auto-close as stale and move on, (b) bring
   it to the next critic cycle as "self-rejected by absence of merge."
   Option (b) is more interesting but harder to get right. Defer.

4. **Worktree hosting — separate clone or `git worktree`?**
   I've assumed `git worktree add` from the user's main checkout. If
   the user runs the loop on Option C (Anthropic infra) they'll need a
   fresh clone per cycle. This is a config knob, not a hard decision.

5. **Concept-tag namespace in claude-mem.**
   I've used `auto-loop` as the umbrella tag. If the user's existing
   claude-mem corpus already uses some flavour of `auto`, `loop`, `bot`,
   collide-tag and reroute. Five-minute fix.

6. **What does the loop do with shader vertex counterparts (`.vs`)?**
   Some manifest entries have `hasVertex: true` (e.g. id 11 Dot Sphere).
   For new generators the loop should default to `hasVertex: false`
   and stick to fragment-only ISF, matching the recent art-movement
   entries. Re-evaluate if the loop ever proposes a particle shader.

7. **Web access in the researcher subagent.**
   `WebFetch` and `WebSearch` are available in this harness. On
   Option D (local launchd) they need to be invoked from a Claude
   Code session that has them enabled by default. Confirm those tools
   are in the auto-allowed set in `.claude/settings.json` for the loop
   to run unattended; otherwise it'll prompt and block forever. The
   `fewer-permission-prompts` skill can audit this.

8. **Idempotency on cron retries.**
   If `launchd` retries a failed cycle within the same minute, the
   worktree path collides. Salt with seconds, or guard with a
   `flock`-style lockfile.

---

## 7. Cross-cutting implementation notes

- **Worktree path:** `~/easel-autoloop/worktrees/<YYYY-MM-DD-HHMM>/`.
  Salted with HHMM to avoid retry collisions (§6.8).
- **Cycle log:** every cycle writes a full transcript to
  `.planning/notes/auto_loop/log/<YYYY-MM-DD>.md` (committed only on
  PR-opening cycles to keep `main`'s history clean — non-PR cycle logs
  live in the worktree and are discarded with it; the digest line
  preserves the summary).
- **Branch name:** `auto-loop/<YYYY-MM-DD>-<topic-slug>`, e.g.
  `auto-loop/2026-05-12-suprematism-malevich`.
- **PR labels:** every PR gets the `auto-loop` GH label, plus one of
  `auto-loop/parameter-add` or `auto-loop/new-shader`. The 3-per-week
  cap queries by the umbrella `auto-loop` label.
- **Manifest ID allocation:** writer reads
  `external/ShaderClaw3/shaders/manifest.json`, computes
  `max(entries[].id) + 1`. No gaps allowed; if the user has reserved a
  range, they say so in `.planning/notes/auto_loop/config.toml`.
- **GLSL helpers:** the writer must reuse `_lib.glsl` helpers
  (`worleyFbm`, `curlField`, `fauvistPalette`, etc., enumerated in
  `art_movement_shaders.md` §"Cross-cutting implementation notes").
  Inventing a new helper is allowed but must come with the helper
  added to `_lib.glsl` in the same PR.
- **Audio idle floor:** every new shader must `max(audioX, 0.05)` for
  at least one channel, matching the convention from the art-movement
  series. The exception is shaders explicitly designed to be silent
  (Rothko-class colour-fields).
- **Style:** new `.fs` files match the formatting of
  `external/ShaderClaw3/shaders/cubism_braque.fs` — 4-space indent,
  `vec3` palette constants near the top, `main()` last.
- **No CMake builds during the cycle.** The loop never invokes
  `cmake --build`; it only edits `.fs` and `manifest.json`. Build
  validation is the user's job at PR-review time. (This restriction
  preserves the loop's runtime budget and prevents broken cmake state
  in the worktree from blocking future cycles.)

---

## 8. Sample cycle (concrete walkthrough)

Date: 2026-05-12, 04:00 PT. Loop fires.

**04:00:01** — launchd executes `claude --print`. Worktree created at
`~/easel-autoloop/worktrees/2026-05-12-0400/` from `main`. Branch
`auto-loop/2026-05-12-pending` checked out. PAUSE file absent. PR-cap
check: `gh pr list --label auto-loop --state open` returns 1 (still
within the 3-per-week cap). Proceed.

**04:00:30** — researcher subagent starts. Queries claude-mem for
recent corpus cursor: alphabet rotation is at "S" for art movements,
"B" for techniques. WebSearches "Suprematism Malevich Black Square
1915 visual vocabulary"; fetches Tate Modern overview, Wikipedia,
one MoMA collection page. Also checks ShaderToy front page (3 entries,
all raymarched scenes — discounts as too expensive). Produces 4
candidates: SUPREMATISM_MALEVICH, BITANGENT_NOISE_PRIMITIVE (technique),
SHADERTOY_TODAY_KIFS_FOREST, AGX_TONEMAP. Writes
`/tmp/auto-loop/2026-05-12-0400/candidates.md`.

**04:02:15** — critic subagent starts. Reads candidates +
`manifest.json` + corpus search per candidate. Verdicts:

- SUPREMATISM_MALEVICH → *new-shader*, justified (no overlap with
  MINIMALISM_STELLA or DESTIJL_MONDRIAN — both already cataloged in
  corpus and distinct).
- BITANGENT_NOISE_PRIMITIVE → *parameter-add* candidate; note
  AI_LATENT_DRIFT (when shipped) and STAINED_GLASS_CHARTRES could
  benefit, but neither lives yet — *defer* (corpus entry as candidate).
- SHADERTOY_TODAY_KIFS_FOREST → *reject* (corpus already has CRYSTAL_CUBES
  and similar, vibe overlap).
- AGX_TONEMAP → *reject for this loop*, route to a separate
  cross-cutting refactor PR (it's a host-side change, not a shader).

Winners: 1 (SUPREMATISM_MALEVICH). Writes
`/tmp/auto-loop/2026-05-12-0400/critique.md`.

**04:04:00** — writer subagent starts. Drafts
`external/ShaderClaw3/shaders/suprematism_malevich.fs` (78 lines,
8 INPUTs including `inputTex` for "implied ground texture"). Appends
manifest entry id 477. Commits, pushes branch, opens PR titled
`auto-loop: 2026-05-12 — Suprematism (Malevich)`. PR body includes
critique log.

**04:05:30** — cycle wraps. Digest line appended:

```
2026-05-12 04:05 — researched 4 → 1 winner (Suprematism), 2 rejects,
  1 deferred → PR #143 opened
```

Worktree retained for 7 days then auto-pruned by a separate
`launchd` plist entry.

**The user wakes up at 8am.** Reads the digest line. Opens PR #143 on
the train. Reviews the GLSL, builds locally to confirm it loads, merges
or comments. If they merge, the next cycle's corpus naturally records
"Suprematism = shipped, manifest id 477." If they close-without-merge,
the corpus records "Suprematism = rejected by user", and the loop
won't re-propose it.

That's the loop. Most days it ships nothing. Some days it ships a
shader. Over a year, the library compounds in a way the user couldn't
do by hand without giving up the rest of their work.

---

## 9. Build order

If the user actually wants to build this thing, ship in this sequence
(easy → hard, each phase usable on its own):

1. **Skeleton + kill switch + digest.** A launchd plist that runs a
   no-op `claude --print "echo cycle ran"` and appends to the digest.
   Validates the cron, the worktree creation, the PAUSE file behaviour.
   Half a day's work.
2. **Researcher only.** Add the researcher subagent. Output candidates
   to a file. Don't critique yet, don't open PRs. Run for a week.
   Read the candidate slates each morning to gut-check the source mix.
3. **Critic.** Add `manifest.json` parsing and the critique step. Still
   no PRs — write critiques to digest only. Run for a week.
   This is the moment the loop earns trust or doesn't.
4. **Writer (parameter-add only).** Enable PR opening, but only for
   parameter-add path (smaller diffs, lower stakes). Cap at 1 PR/week.
5. **Writer (new-shader path).** Enable full new-shader generation.
   Move cap to 3 PRs/week.
6. **Corpus migration to claude-mem.** Until step 5 the corpus can
   live in flat files in `.planning/notes/auto_loop/corpus/`. Migrate
   once the loop has shipped its first merged PR — that's when the
   compounding-knowledge benefit kicks in.
7. **Visual sanity render.** Add the PNG attachment to PRs once
   `easel-render` exists.
8. **Migrate to remote (Option C).** Once ≥30 days of stable cycles,
   move from launchd to Anthropic-hosted scheduled agent so it runs
   while the laptop is closed.

---

## 10. What I deliberately did not specify

A short list of things I considered and dropped, in case they tempt me
later:

- **Multi-armed-bandit candidate scoring.** The alphabet rotation is
  dumb-but-correct. A bandit that learns "user merges Cubism PRs more
  than Pop-Art PRs" is *plausibly* better but adds state I don't trust
  the loop to maintain over months. Revisit at year 1.
- **Auto-grading via image embeddings.** Rendering the PR's `.fs` and
  CLIP-embedding it against the reference work to score authenticity
  would be cool. Out of scope until the basic loop ships and earns
  trust.
- **Cross-shader composition proposals.** "Combine FAUVISM with
  GLITCH_DATAMOSH for a wild new effect" is a lovely thought and
  probably wrong 90% of the time. Leave it to the user.
- **Self-modifying loop.** The loop opening PRs against its own prompt
  files is the obvious meta-move and the obvious failure mode. Hard
  rule: the loop never edits files inside `.planning/notes/auto_loop/`.

---

## Sources

- [launchd.info — plist reference](https://www.launchd.info/)
- [GitHub Actions — schedule events](https://docs.github.com/en/actions/writing-workflows/choosing-when-your-workflow-runs/events-that-trigger-workflows#schedule)
- [Anthropic Claude Agent SDK overview](https://docs.anthropic.com/en/docs/agent-sdk/overview)
- [Anthropic Claude Agent SDK — subagents](https://docs.anthropic.com/en/docs/agent-sdk/subagents)
- [`gh pr create` reference](https://cli.github.com/manual/gh_pr_create)
- [`git worktree` man page](https://git-scm.com/docs/git-worktree)
- [claude-mem on GitHub — thedotmack/claude-mem](https://github.com/thedotmack/claude-mem)
- [arXiv cs.GR recent feed](https://arxiv.org/list/cs.GR/recent)
- [ShaderToy](https://www.shadertoy.com/)
- [Two Minute Papers (YouTube)](https://www.youtube.com/@TwoMinutePapers)
- Sibling notes:
  [`art_movement_shaders.md`](./art_movement_shaders.md),
  [`state_of_realtime_2026.md`](./state_of_realtime_2026.md),
  [`15_shader_specs.md`](./15_shader_specs.md)
- Manifest ground truth:
  [`external/ShaderClaw3/shaders/manifest.json`](../../external/ShaderClaw3/shaders/manifest.json)
- Style reference for new `.fs`:
  [`external/ShaderClaw3/shaders/cubism_braque.fs`](../../external/ShaderClaw3/shaders/cubism_braque.fs),
  [`art_nouveau_mucha.fs`](../../external/ShaderClaw3/shaders/art_nouveau_mucha.fs),
  [`fauvism_matisse.fs`](../../external/ShaderClaw3/shaders/fauvism_matisse.fs)
