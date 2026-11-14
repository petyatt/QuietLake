#include "LakeSwimmer.h"

namespace
{
    constexpr float H = 1.0f/240;
    constexpr float Mass[] = {0.12f,0.25f,0.28f,0.20f,0.10f,0.05f};
    FVector Perp(FVector A) { return FVector(-A.Y,A.X,0); }
}
void FLakeSwimmer::Reset(FVector Mouth, float BodyLength)
{
    Length=FMath::Max(10.0f,BodyLength);
    Time=Accumulator=Drive=Bend=0;
    for(int I=0;I<Count;++I) { P[I]=Mouth-FVector(Length*I/(Count-1),0,0); V[I]=FVector::ZeroVector; }
}
FVector FLakeSwimmer::Centre() const
{
    FVector Sum=FVector::ZeroVector;
    for(int I=0;I<Count;++I) Sum+=P[I]*Mass[I];
    return Sum;
}
FVector FLakeSwimmer::Velocity() const
{
    FVector Sum=FVector::ZeroVector;
    for(int I=0;I<Count;++I) Sum+=V[I]*Mass[I];
    return Sum;
}
void FLakeSwimmer::Advance(float DeltaSeconds, float Effort, float Steering)
{
    Accumulator+=FMath::Clamp(DeltaSeconds,0.0f,0.1f);
    while(Accumulator>=H) { Step(Effort,Steering); Accumulator-=H; }
}
void FLakeSwimmer::Step(float Effort, float Steering)
{
    Time+=H;
    Drive=FMath::FInterpTo(Drive,FMath::Clamp(Effort,0.0f,1.0f),H,4);
    Bend=FMath::FInterpTo(Bend,FMath::Clamp(Steering,-1.0f,1.0f),H,3);
    FVector Before[Count];
    const float Segment=Length/(Count-1);
    for(int I=0;I<Count;++I)
    {
        Before[I]=P[I];
        const FVector Axis=(P[FMath::Min(I+1,Count-1)]-P[FMath::Max(0,I-1)]).GetSafeNormal();
        const FVector Along=Axis*FVector::DotProduct(V[I],Axis);
        // No forward thrust term: sideways strokes meet more resistance than gliding.
        if(bWater) V[I]=Along*FMath::Exp(-1.2f*H)+(V[I]-Along)*FMath::Exp(-22.0f*H);
        P[I]+=V[I]*H;
    }
    float MuscleLambda[Count]={};
    for(int Pass=0;Pass<12;++Pass)
    {
        for(int I=0;I<Count-1;++I)
        {
            const FVector D=P[I+1]-P[I];
            const float Distance=D.Size();
            if(Distance<0.001f) continue;
            const FVector Fix=D/Distance*((Distance-Segment)/(1/Mass[I]+1/Mass[I+1]));
            P[I]+=Fix/Mass[I]; P[I+1]-=Fix/Mass[I+1];
        }
        for(int I=1;I<Count-1;++I)
        {
            const FVector A=P[I]-P[I-1], B=P[I+1]-P[I];
            const float Angle=FMath::Atan2(float(FVector::CrossProduct(A,B).Z),float(FVector::DotProduct(A,B)));
            const float Tail=float(I)/(Count-2);
            const float Wanted=Drive*(0.12f+0.48f*Tail)*FMath::Sin(Time*11-I*1.05f)+Bend*0.16f;
            const float Error=FMath::UnwindRadians(Angle-Wanted);
            const FVector G0=Perp(A)/FMath::Max(0.01,A.SizeSquared());
            const FVector G2=Perp(B)/FMath::Max(0.01,B.SizeSquared());
            const FVector G1=-G0-G2;
            const float Alpha=0.000002f/(H*H);
            const float Denom=G0.SizeSquared()/Mass[I-1]+G1.SizeSquared()/Mass[I]+G2.SizeSquared()/Mass[I+1]+Alpha;
            const float Lambda=(-Error-Alpha*MuscleLambda[I])/Denom;
            MuscleLambda[I]+=Lambda;
            P[I-1]+=G0*(Lambda/Mass[I-1]); P[I]+=G1*(Lambda/Mass[I]); P[I+1]+=G2*(Lambda/Mass[I+1]);
        }
    }
    for(int I=0;I<Count;++I) V[I]=(P[I]-Before[I])/H;
}
