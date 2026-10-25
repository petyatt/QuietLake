#include "FisherAnim.h"
#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Components/SkeletalMeshComponent.h"
#include "TwoBoneIK.h"

namespace
{
    // Reference speeds of the UE5 mannequin locomotion clips (cm/s).
    constexpr float WalkClipSpeed = 165.0f, JogClipSpeed = 400.0f;
}

void UFisherAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    const TCHAR* Root = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/");
    IdleSequence = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%sMM_Idle.MM_Idle"), Root));
    WalkSequence = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%sWalk/MF_Unarmed_Walk_Fwd.MF_Unarmed_Walk_Fwd"), Root));
    JogSequence = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%sJog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd"), Root));
    if (!IdleSequence || !WalkSequence || !JogSequence)
        UE_LOG(LogTemp, Warning, TEXT("Fisher animations missing: run Tools/CopyMannequin.ps1"));
}

void FFisherAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
    FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
    const UFisherAnimInstance* Instance = CastChecked<UFisherAnimInstance>(InAnimInstance);
    Idle = Instance->IdleSequence;
    Walk = Instance->WalkSequence;
    Jog = Instance->JogSequence;
    Input = Instance->PoseInput;
    // The worker thread works in component space: convert the world-space grips now.
    const FTransform ToComponent = Instance->GetSkelMeshComponent()->GetComponentTransform().Inverse();
    auto Convert = [&ToComponent](const FHandGrip& G)
    {
        FCsGrip C;
        C.Point = ToComponent.TransformPosition(G.Point);
        C.Axis = ToComponent.TransformVectorNoScale(G.Axis).GetSafeNormal();
        C.Down = ToComponent.TransformVectorNoScale(G.Down).GetSafeNormal();
        C.Pole = ToComponent.TransformPosition(G.Pole);
        C.Alpha = G.Alpha;
        return C;
    };
    Right = Convert(Input.Right);
    Left = Convert(Input.Left);
}

void FFisherAnimProxy::Update(float DeltaSeconds)
{
    // Advance clip times; walking clips play in step with the actual ground speed.
    IdleTime += DeltaSeconds;
    const float Rate = FMath::Max(Input.Speed, 1.0f);
    WalkTime += DeltaSeconds * FMath::Clamp(Rate / WalkClipSpeed, 0.5f, 1.6f);
    JogTime += DeltaSeconds * FMath::Clamp(Rate / JogClipSpeed, 0.5f, 1.4f);
}

void FFisherAnimProxy::Sample(const UAnimSequence* Sequence, float Time, FPoseContext& Pose) const
{
    if (!Sequence) { Pose.ResetToRefPose(); return; }
    FAnimationPoseData Data(Pose);
    const float Length = FMath::Max(Sequence->GetPlayLength(), 0.01f);
    Sequence->GetAnimationPose(Data, FAnimExtractContext(FMath::Fmod(Time, Length), false, {}, true));
}

void FFisherAnimProxy::SolveArm(FComponentSpacePoseContext& Pose, FName Upper, FName Lower, FName Hand,
                                FName Index, FName Middle, FName Pinky, const FCsGrip& Grip) const
{
    if (Grip.Alpha <= 0.001f) return;
    const FBoneContainer& Bones = Pose.Pose.GetPose().GetBoneContainer();
    auto Bone = [&Bones](FName Name)
    {
        const int32 Mesh = Bones.GetPoseBoneIndexForBoneName(Name);
        return Mesh == INDEX_NONE ? FCompactPoseBoneIndex(INDEX_NONE) : Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Mesh));
    };
    const FCompactPoseBoneIndex U = Bone(Upper), L = Bone(Lower), H = Bone(Hand), I = Bone(Index), M = Bone(Middle), P = Bone(Pinky);
    if (!U.IsValid() || !L.IsValid() || !H.IsValid() || !I.IsValid() || !M.IsValid() || !P.IsValid()) return;

    FTransform UT = Pose.Pose.GetComponentSpaceTransform(U);
    FTransform LT = Pose.Pose.GetComponentSpaceTransform(L);
    FTransform HT = Pose.Pose.GetComponentSpaceTransform(H);
    const FTransform U0 = UT, L0 = LT, H0 = HT;

    // Palm frame of the animated hand: F = towards the fingers, S = across the palm (pinky -> index).
    const FVector Wrist = HT.GetLocation();
    const FVector F = (Pose.Pose.GetComponentSpaceTransform(M).GetLocation() - Wrist).GetSafeNormal();
    const FVector S = (Pose.Pose.GetComponentSpaceTransform(I).GetLocation() - Pose.Pose.GetComponentSpaceTransform(P).GetLocation()).GetSafeNormal();
    const float PalmLength = FVector::Dist(Wrist, Pose.Pose.GetComponentSpaceTransform(M).GetLocation());

    // Desired palm frame: the index side points along the rod, the fingers wrap downwards around it.
    const FVector S2 = Grip.Axis;
    const FVector F2 = (Grip.Down - S2 * FVector::DotProduct(Grip.Down, S2)).GetSafeNormal();
    const FQuat Current = FRotationMatrix::MakeFromXY(F, S).ToQuat();
    const FQuat Wanted = FRotationMatrix::MakeFromXY(F2, S2).ToQuat();
    const FQuat Delta = Wanted * Current.Inverse();

    // The wrist sits behind the palm centre, a little to the side of the rod.
    const FVector N2 = FVector::CrossProduct(F2, S2);
    const FVector WristTarget = Grip.Point - F2 * PalmLength * 0.55f - N2 * 2.5f;
    AnimationCore::SolveTwoBoneIK(UT, LT, HT, Grip.Pole, WristTarget, false, 1.0, 1.15);
    HT.SetRotation(Delta * H0.GetRotation());

    auto Blend = [&Grip](const FTransform& A, const FTransform& B)
    {
        FTransform R;
        R.Blend(A, B, Grip.Alpha);
        return R;
    };
    TArray<FBoneTransform> Result = { FBoneTransform(U, Blend(U0, UT)), FBoneTransform(L, Blend(L0, LT)), FBoneTransform(H, Blend(H0, HT)) };
    Result.Sort(FCompareBoneTransformIndex());
    Pose.Pose.SafeSetCSBoneTransforms(Result);
}

bool FFisherAnimProxy::Evaluate(FPoseContext& Output)
{
    // --- locomotion: idle -> walk -> jog by ground speed ---
    Sample(Idle, IdleTime, Output);
    const float WalkAlpha = FMath::Clamp(Input.Speed / 120.0f, 0.0f, 1.0f);
    const float JogAlpha = FMath::Clamp((Input.Speed - 260.0f) / 140.0f, 0.0f, 1.0f);
    if (WalkAlpha > 0.01f)
    {
        FPoseContext Moving(Output), Blended(Output);
        Sample(Walk, WalkTime, Moving);
        if (JogAlpha > 0.01f)
        {
            FPoseContext Running(Output), Mixed(Output);
            Sample(Jog, JogTime, Running);
            FAnimationPoseData MixedData(Mixed);
            FAnimationRuntime::BlendTwoPosesTogether(FAnimationPoseData(Moving), FAnimationPoseData(Running), 1 - JogAlpha, MixedData);
            Moving.Pose.CopyBonesFrom(Mixed.Pose);
        }
        FAnimationPoseData BlendedData(Blended);
        FAnimationRuntime::BlendTwoPosesTogether(FAnimationPoseData(Output), FAnimationPoseData(Moving), 1 - WalkAlpha, BlendedData);
        Output.Pose.CopyBonesFrom(Blended.Pose);
        Output.Curve.CopyFrom(Blended.Curve);
    }

    // --- upper body in component space ---
    FComponentSpacePoseContext CS(this);
    CS.Pose.InitPose(Output.Pose);
    const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
    // Lean the spine with the view pitch so the arms and the first-person camera agree.
    // The mannequin faces +Y in component space, so pitch is a rotation about the X axis.
    const float Pitch = FMath::Clamp(Input.AimPitch, -60.0f, 60.0f);
    for (const TPair<FName, float>& Spine : { TPair<FName, float>(TEXT("spine_03"), 0.25f), TPair<FName, float>(TEXT("spine_05"), 0.2f) })
    {
        const int32 Mesh = Bones.GetPoseBoneIndexForBoneName(Spine.Key);
        if (Mesh == INDEX_NONE) continue;
        const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Mesh));
        FTransform T = CS.Pose.GetComponentSpaceTransform(Index);
        T.SetRotation(FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(Pitch * Spine.Value)) * T.GetRotation());
        CS.Pose.SafeSetCSBoneTransforms({ FBoneTransform(Index, T) });
    }
    SolveArm(CS, TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"), TEXT("index_01_r"), TEXT("middle_01_r"), TEXT("pinky_01_r"), Right);
    SolveArm(CS, TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"), TEXT("index_01_l"), TEXT("middle_01_l"), TEXT("pinky_01_l"), Left);
    FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(CS.Pose, Output.Pose);
    return true;
}
