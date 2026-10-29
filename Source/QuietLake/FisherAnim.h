#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "FisherAnim.generated.h"

/** Where a hand should grip the rod this frame (world space). */
struct FHandGrip
{
    FVector Point = FVector::ZeroVector;   // centre of the palm
    FVector Axis = FVector::ForwardVector; // rod direction through the palm (towards the tip)
    FVector Down = -FVector::UpVector;     // direction the fingers wrap towards
    FVector Pole = FVector::ZeroVector;    // where the elbow should point
    float Alpha = 0;                       // 0 = animation only, 1 = hand fully on the grip
    float Pinch = 0;                       // 0 = support the rod, 1 = pinch the line
};

/** Game-thread inputs, filled by the character every frame. */
struct FFisherPoseInput
{
    float Speed = 0;        // ground speed, cm/s
    float AimPitch = 0;     // degrees, + looks up
    FHandGrip Right, Left;
};

/**
 * Animation proxy evaluated on the worker thread. Pure C++, no animation blueprint:
 * blends idle / walk / jog by speed, leans the upper spine with the aim pitch and
 * puts both hands on the rod with two-bone IK. The palm is oriented from the actual finger
 * bones of the pose, so the grip does not depend on the hand bone's axis convention.
 */
USTRUCT()
struct FFisherAnimProxy : public FAnimInstanceProxy
{
    GENERATED_BODY()
    FFisherAnimProxy() {}
    FFisherAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

    virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
    virtual void Update(float DeltaSeconds) override;
    virtual bool Evaluate(FPoseContext& Output) override;

private:
    struct FCsGrip { FVector Point, Axis, Down, Pole; float Alpha = 0, Pinch = 0; };
    void Sample(const class UAnimSequence* Sequence, float Time, FPoseContext& Pose) const;
    void SolveArm(FComponentSpacePoseContext& Pose, FName Upper, FName Lower, FName Hand,
                  FName Index, FName Middle, FName Pinky, const FCsGrip& Grip) const;
    /** Close the fingers (and the thumb) of one hand around what it holds, Amount 0..1. */
    void CurlFingers(FCompactPose& Pose, const TCHAR* Side, float Sign, float Amount, float Pinch = 0) const;

    const UAnimSequence* Idle = nullptr;
    const UAnimSequence* Walk = nullptr;
    const UAnimSequence* Jog = nullptr;
    FFisherPoseInput Input;
    FCsGrip Right, Left;
    float IdleTime = 0, WalkTime = 0, JogTime = 0;
};

UCLASS(Transient, NotBlueprintable)
class QUIETLAKE_API UFisherAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    virtual void NativeInitializeAnimation() override;
    FFisherPoseInput PoseInput;

    UPROPERTY() TObjectPtr<UAnimSequence> IdleSequence;
    UPROPERTY() TObjectPtr<UAnimSequence> WalkSequence;
    UPROPERTY() TObjectPtr<UAnimSequence> JogSequence;

protected:
    virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FFisherAnimProxy(this); }
    virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override { delete static_cast<FFisherAnimProxy*>(Proxy); }
};
