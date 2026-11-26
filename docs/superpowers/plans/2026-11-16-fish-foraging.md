# Local bait discovery and distributed feeding implementation plan

**Goal:** Fish discover hook bait locally, feed across a groundbait patch, and do not converge through each other onto a hook.

**Design approved in conversation:** Groundbait draws suitable hungry fish gradually to an area; feeding consumes food and reduces hunger. Hook bait is a separate nearby discovery. One fish approaches each rig; neighbours keep feeding or give way. Preserve fish schooling and hooked-fish physics.

**Architecture:** Keep the existing state machine, food field and server authority. Add local perception and approach arbitration to `LakeFish`, use the spatial grid for neighbours, and distribute feeding destinations across a food patch rather than its centre. Existing groundbait scent supplies gradual attraction; distant fish follow a local scent gradient before locating the patch.

**Tech stack:** Unreal Engine 5.8 C++, existing Unreal automation tests.

**Constraints:** Work in master as requested. No new physics collision bodies. Do not change hooked-fish movement or network protocols. Numerical perception distances are gameplay tuning, not biological claims. Execute in the existing checkout; user approved implementation.

## Review focus

- Multiple fish and multiple rigs: independent approach arbitration per angler and rig.
- Target removed, scared away or hooked: another fish can approach without a stale reservation.
- Fish behind bait, wrong feeding layer or across vegetation: reduced or blocked discovery.
- Schools and groundbait near a bank: individual valid feeding locations, no false feeding from a distance.
- Saturation and depleted food: no permanent attraction or infinite appetite.

## Tasks

- [x] Add regression tests for distant/behind/vertical bait discovery, single approach per rig, separation during approach, and feeding spread/saturation.
- [x] Run the new tests against current behaviour and record the failures.
- [x] Implement local perception, per-rig arbitration and neighbour avoidance during approach; preserve bite contact mechanics.
- [x] Implement distributed feeding and incremental scent following, preserving consumption and hunger.
- [x] Build QuietLakeEditor and run new tests plus the existing fishing suite; investigate failures before claiming completion.
- [x] Review the final diff, record verification and remaining limits.

## Execution record

Initial inspection: detection checks every 0.6–1.4 s with 3.5–8 m radius, then targets exact hook coordinates. Approach omits `SteerSchool` separation. Groundbait targets a +/-90 cm square and exposes its exact centre even at faint scent. User authorized the combined change.

Approved addition: smaller fish react sooner and can steal bait; a clearly dominant fish may displace an approaching or nibbling fish locally, but never a holding or hooked fish. Predation fear remains separate from ordinary competition. Avoid a scripted small-then-large sequence.

Approved addition: audit placement and predator movement, bias mature size groups toward deeper water farther from shore, preserve occasional shallow feeding and hunting visits. The shared habitat score now supplies this preference to stocking and roaming; fallback restocking retains size information.

Ruling: implement in the existing master checkout, following the user's explicit branch instruction and repeated authorization to proceed; no branch or worktree creation, push or merge.

Ruling: occasional theft requires one replicated bait-presence flag in the existing character/rod state (no new RPC or network architecture), otherwise the hook would continue attracting fish and show food after theft. Both parked and held rods preserve emptiness; fresh casting puts on bait.

Verification: `Saved/Logs/Foraging-Red-Tests.log` reproduces the initial failures. Theft independently reproduced 0/96 before implementation, then 9/96 with fixed seeds. `Saved/Logs/Foraging-Final-Build.log`: successful editor build. `Saved/Logs/Foraging-Final-Tests.log`: all 35 QuietLake.Fishing tests passed, including original Groundbait, Stocking, BanksAndDepth, Interactions, ServerRules and ThreeRods plus the four new tests.

Review fixes: displaced investigators explicitly release their target; distant groundbait scent search cannot trigger rooting or consumption; idle rods restore their own bait-presence flag. The casting fixture now has a fixed fish seed and accepts fish that already returned to swimming before its release measurement; hooked-fish physics was not changed. Bite survey fixtures clear the previous stationary fish and start locally interested fish, with perception tested independently.

Limit: automated, headless simulation verification; no visual playtest or frame-time benchmark. Perception uses sampled lake geometry/vegetation, not a new physically based optical or scent simulation. Parameter tuning is intentionally frozen at the user's request.

Final network verification: `Tools/TestNetwork.ps1` exited 0; two clients joined, received fish and passed the motion smoothness and initial rod loadout checks (`Saved/Logs/Foraging-Network.log`). This is connection/replication regression coverage, not an end-to-end network bait-theft scenario. Work was completed in master; the user subsequently requested committing and pushing the changes.
