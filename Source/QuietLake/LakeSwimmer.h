#pragma once
#include "CoreMinimal.h"

/**
 * Experimental free swimmer: a chain of six points from the mouth to the tail fin, all of them free.
 * Muscles only bend the joints (internal forces, limited torque); the fish moves because the water
 * holds its body sideways far better than lengthwise. Units: cm, seconds, grams; forces from outside
 * in gram-force.
 *
 * The water, per point (by how much of the point is under the surface):
 *  - carried water: pushing a tall body sideways means moving the water beside it, so against a
 *    sideways push the point is heavier by a cylinder of water of the body's height there (up and
 *    down: of its width);
 *  - resistance grows with the square of speed: across the body by its side area, up and down by its
 *    area from above, along it by its wetted area (a backward slide meets far more);
 *  - the tail fin's trailing edge leaves the water it pushed sideways behind as a wake;
 *  - it carries the fish's weight: under water the fish neither sinks nor floats up, a part above the
 *    surface weighs what it weighs.
 * Speed is set by the beat rate (Effort), as in a fish; the bend of the stroke hardly changes.
 * A line can hold the fish by the mouth (LineTip, LineLength): it pulls only when taut.
 */
struct FLakeSwimmer
{
    static constexpr int32 Count = 6;
    FVector P[Count], V[Count];
    // The fish's back: the stroke bends the body across it. In the water the fish rights itself.
    FVector Back = FVector::UpVector;
    // Where the fish wants its back (zero: up). To turn hard up or down it banks: lies over on its
    // side, so that the strong sideways bend of its body works in the plane of the turn.
    FVector WantBack = FVector::ZeroVector;
    float Length = 30, Grams = 350, Time = 0, Accumulator = 0;
    float Drive = 0, Bend = 0, Lift = 0, Phase = 0;
    // Tail beats per second now, and the share of the joints whose muscles are at their limit.
    float Beat = 0, MuscleUse = 0;
    // How much of each point is under water (0..1).
    float Wet[Count];
    // Scales the muscles' torque limit: a stronger or a tired fish.
    float Strength = 1;
    // Height of the water's surface and of the bottom.
    float Surface = 1e9f, Floor = -1e9f;
    // A pull on the mouth from outside, gram-force. If it comes from a spring (a rod), MouthSpring is
    // its stiffness, gram-force per centimetre: as the mouth gives way along the pull from where it
    // was when the pull was measured (MouthAt), the pull eases by that much, and never turns round.
    FVector MouthForce = FVector::ZeroVector;
    float MouthSpring = 0;
    FVector MouthAt = FVector::ZeroVector;
    // Scales the fastest beat: a stronger fish for its length beats faster and pulls harder (the
    // pull grows with the square of the beat rate).
    float BeatScale = 1;
    // A line from LineTip to the mouth, LineLength long (0: no line); it stretches by a centimetre
    // per LineStiffness gram-force. LineTension is what it pulls with now, gram-force.
    FVector LineTip = FVector::ZeroVector;
    float LineLength = 0, LineStiffness = 40, LineTension = 0;
    bool bWater = true;

    FLakeSwimmer() { Reset(); }
    // BodyHeight: the body's greatest height as a share of its length (a perch 0.27, a pike 0.15).
    void Reset(FVector Mouth = FVector::ZeroVector, float BodyLength = 30, float BodyGrams = 350, FVector Forward = FVector::ForwardVector, float BodyHeight = 0.27f);
    // Effort 0..1: none, an easy swim around 0.15, a burst at 1. Steering -1..1 bends the body
    // sideways, Climb -1..1 bends it down or up.
    void Advance(float DeltaSeconds, float Effort, float Steering, float Climb = 0);
    FVector Centre() const;
    FVector Velocity() const;
    FVector Forward() const { return (P[0] - P[1]).GetSafeNormal(); }
    // Where the body as a whole points: its front three fifths, not the head alone.
    FVector Heading() const { return (P[0] - P[3]).GetSafeNormal(); }
    // Segment I (0 = the head) as a direction from the mouth towards the tail.
    FVector SegmentDir(int32 I) const { return (P[I + 1] - P[I]).GetSafeNormal(); }
    // The fastest tail beat of a fish this long, per second: small fish beat faster. (No faster than
    // the fixed step can follow.)
    float MaxBeat() const { return FMath::Min(24.0f, 55.0f / FMath::Sqrt(Length) * BeatScale); }
    // Gives every point this velocity: the fish as a whole is thrown or carried.
    void SetVelocity(const FVector& Velocity) { for (FVector& Point : V) Point = Velocity; }
    // Moves the whole fish.
    void Shift(const FVector& Delta) { for (FVector& Point : P) Point += Delta; }
    // Share of the fish's weight that is under water (0..1).
    float WaterShare() const;
    /**
     * How the fish bends to point itself along Want: Steering and Climb for Advance. It turns across
     * its back with the sideways bend of its body and along it with its tail and fins, which is much
     * the weaker; so to turn hard up or down (a line drawing its head up while it means to bore down)
     * it banks: rolls over on its side (WantBack), and bends round with all its body. Eased off as
     * it comes round, so that it does not swing past.
     */
    void SteerTo(const FVector& Want, float DeltaSeconds, float& Steering, float& Climb);
    // Where the body points, smoothed over the stroke (which swings it a few degrees every beat).
    FVector Steady = FVector::ForwardVector;

private:
    void Step(float Effort, float Steering, float Climb);
    float Mass[Count], Tall[Count], Wide[Count], CarriedSide[Count], CarriedUp[Count], SideArea[Count], TopArea[Count], Along[Count];
    float ShedMass = 0;
};
