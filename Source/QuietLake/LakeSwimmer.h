#pragma once
#include "CoreMinimal.h"

/**
 * Experimental free swimmer: a chain of six points from the mouth to the tail fin, all of them free.
 * Muscles only bend the joints (internal forces, limited torque); the fish moves because the water
 * holds its body sideways far better than lengthwise. Planar, neutrally buoyant, fully submerged.
 * Units: cm, seconds, grams; forces from outside in gram-force.
 *
 * The water, per point:
 *  - carried water: pushing a tall body sideways means moving the water beside it, so against a
 *    sideways push the point is heavier by a cylinder of water of the body's height there;
 *  - resistance grows with the square of speed: across the body by its side area, along it by its
 *    wetted area (a backward slide meets far more);
 *  - the tail fin's trailing edge leaves the water it pushed sideways behind as a wake.
 * Speed is set by the beat rate (Effort), as in a fish; the bend of the stroke hardly changes.
 */
struct FLakeSwimmer
{
    static constexpr int32 Count = 6;
    FVector P[Count], V[Count];
    float Length = 30, Grams = 350, Time = 0, Accumulator = 0;
    float Drive = 0, Bend = 0, Phase = 0;
    // Tail beats per second now, and the share of the joints whose muscles are at their limit.
    float Beat = 0, MuscleUse = 0;
    // Scales the muscles' torque limit: a stronger or a tired fish.
    float Strength = 1;
    // A pull on the mouth from outside, gram-force (a line).
    FVector MouthForce = FVector::ZeroVector;
    bool bWater = true;

    FLakeSwimmer() { Reset(); }
    void Reset(FVector Mouth = FVector::ZeroVector, float BodyLength = 30, float BodyGrams = 350);
    // Effort 0..1: none, an easy swim around 0.15, a burst at 1. Steering -1..1 bends the body.
    void Advance(float DeltaSeconds, float Effort, float Steering);
    FVector Centre() const;
    FVector Velocity() const;
    FVector Forward() const { return (P[0] - P[1]).GetSafeNormal(); }
    // The fastest tail beat of a fish this long, per second: small fish beat faster.
    float MaxBeat() const { return 55.0f / FMath::Sqrt(Length); }

private:
    void Step(float Effort, float Steering);
    float Mass[Count], Carried[Count], Across[Count], Along[Count];
    FVector Normal[Count];
    float Shed = 0;
};
