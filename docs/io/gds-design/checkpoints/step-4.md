# Checkpoint: step 4, design counsel

- Status: DONE
- Input: `04-counsel-input.md` (brief plus appendices `01`, `02`, `03`)
- Session: `~/.pi/counsel/20261117-122150-gds-reactor-design/`. It holds the drafts, the thinking transcripts, and the judge files.
- Config: `~/.pi/agent/counsel.json`. Original backed up as `counsel.json.bak-20261117122138`.
  - Judge: claude-fable-5-2 @ max.
  - Experts:
    - `gds-io-expert`: gpt-6-astra
    - `spectrum-scale-expert`: claude-fable-5
    - `weka-vast-expert`: gemini-4-argon
- Result: winner `gds-io-expert` (clear).
  - Scores: gds-io 86, spectrum-scale 75, weka-vast 14. The Gemini draft was truncated by the 4.1K output cap on nvidia-inference.
- Outputs in repo:
  - `05-final-design.md`: the judge's spec, built from the winning plan
  - `05a-judge-evaluation.md`
- Fixes the judge still requires of the winner (not yet applied):
  1. Gate only the destination D2D on the input fence, not the read into staging.
  2. Define a fallback evidence path so `unknown` mounts do not stay on sync forever.
  3. P0 should compare 256 MiB / 2 MiB ops against block-sized ops on GPFS.
  4. Make small-slice coalescing explicit for DISK restore.
  5. Check whether one submitter per device is enough (fewer threads).
- Next: step 5. User Q&A on the design. No coding until approved.
