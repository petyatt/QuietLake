# Village lake water implementation plan

**Goal:** Tune native Water to a subdued rural lake, with depth-dependent visibility and local fishing ripples.
**Architecture:** Project-owned derivative of Unreal Water, retaining native rendering. Connect the existing ripple height texture rather than introducing another simulation. Keep fish physics and terrain unchanged.
**Tech Stack:** UE 5.8, editor Python, C++.
**Spec:** User approved the proposed olive shallows, darker depth, soft wind ripples and fishing rings, delegating artistic choices.

## Constraints and decisions
- Work in master in the existing project; preserve unrelated local changes. No commit or agents requested.
- User opted out of further design questions. Execute inline.
- Configuration and material appearance use asset checks and rendered scenes; do not create implementation-mirroring unit tests.
- Keep engine assets unmodified. Reuse current water-height simulation for floating tackle.

## Tasks
- [ ] Inspect native material parameters and graph inputs. Verify the current absence of local ripple parameters before adding them.
- [ ] Create a reproducible project material setup, tune absorption/scattering and normals, connect existing ripple texture, and assign lake/river/LOD/transition variants consistently.
- [ ] Update runtime material discovery and generator references. Build and check material bindings, render shoreline and fish visibility at depth, run existing ripple checks.
- [ ] Record results and limitations in WORLD.md and README.md.

## Review focus
Unmodified engine assets; fallback materials and river mouths; rings fade at simulation edges; fish fades gradually without artificial disappearance; no added geometric wave motion that disagrees with existing physics.
