#pragma once

#include "CoreMinimal.h"

/**
 * The body of a hooked fish as a flexible spine: six points from the mouth to the tip of the tail,
 * five segments between them. The mouth is placed from outside (where the fish's own swimming or the
 * hook has it); every other point has its share of the fish's weight and volume, so in the water it
 * floats and out of it it hangs, and the water drags it much harder across the body than along it.
 * Drawn by its mouth, the body therefore trails behind it head first; lying at the top with its head
 * held up, the tail floats; hanging, it hangs below the hook. Its muscles bend the joints: a wave runs
 * down the body to the tail as it swims or thrashes, and the front of the body turns the head where
 * the fish wants to go, as far as it can stand against the line (Steer).
 */
struct FFishSpineInput
{
    FVector Mouth = FVector::ZeroVector;
    float Length = 30;             // cm, mouth to tail tip
    float BodyHeight = 0.24f;      // depth of the body as a share of its length (for how deep a part is under)
    FVector BodyUp = FVector::UpVector;   // the fish's back: the muscle wave bends the body across it
    float Effort = 0;              // 0..1: how hard the tail works (swimming, thrashing)
    float Beat = 6;                // rad/s of the tail beat
    FVector Want = FVector::ZeroVector;   // where the fish turns its head (zero: nowhere)
    float Steer = 0;               // 0..1: how far it can turn its head there against the line
    float Stiffness = 1;           // a spent fish is limp, a fresh one holds its body straighter
};

struct FFishSpine
{
    static constexpr int32 Nodes = 6;
    static constexpr int32 Segments = Nodes - 1;
    FVector P[Nodes];
    FVector Prev[Nodes];
    float Wet[Nodes] = {};
    float Seg = 6;
    float Time = 0;
    bool bActive = false;

    /** Lays the body out straight behind the mouth, at rest. */
    void Start(const FVector& Mouth, const FVector& Forward, float Length);
    /** One step: the mouth is where In says; the rest of the body answers. */
    void Step(const FFishSpineInput& In, float DeltaSeconds);
    /** Points from the second point to the mouth: where the head looks. */
    FVector HeadForward() const { return (P[0] - P[1]).GetSafeNormal(); }
    /** How much of the body the water holds (0..1). */
    float WaterShare() const;
    /** Segment I (0 = the head) as a direction from the mouth towards the tail. */
    FVector SegmentDir(int32 I) const { return (P[I + 1] - P[I]).GetSafeNormal(); }
};
