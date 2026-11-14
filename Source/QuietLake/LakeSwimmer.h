#pragma once
#include "CoreMinimal.h"

// Experimental free swimmer. All nodes move; muscles only exert internal joint forces.
// Planar, neutrally buoyant water model with anisotropic resistance (cm, seconds).
struct FLakeSwimmer
{
    static constexpr int32 Count = 6;
    FVector P[Count], V[Count];
    float Length = 30, Time = 0, Accumulator = 0, Drive = 0, Bend = 0;
    bool bWater = true;
    void Reset(FVector Mouth = FVector::ZeroVector, float BodyLength = 30);
    void Advance(float DeltaSeconds, float Effort, float Steering);
    FVector Centre() const;
    FVector Velocity() const;
    FVector Forward() const { return (P[0]-P[1]).GetSafeNormal(); }
private:
    void Step(float Effort, float Steering);
};
