#if WITH_DEV_AUTOMATION_TESTS
#include "LakeTestCommon.h"

// Live Fish stocks the pond: every species in its numbers, a school in reach of every fishing spot,
// and a fish taken from a school comes back into the same school.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeEcosystemPrototypeTest, "QuietLake.Ecosystem.Prototype", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeEcosystemPrototypeTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->StockEcosystemPrototype();
    Life->StockEcosystemPrototype();
    TSet<int32> Ids;
    int32 Pike = 0, Perch = 0, Roach = 0, Crucian = 0;
    for (TActorIterator<ALakeFish> It(World); It; ++It)
    {
        Ids.Add(It->PrototypeId);
        if (It->Species == EFishSpecies::Pike)
            TestEqual(TEXT("Prototype pike starts at 75 percent hunger"), It->GetHunger(), 0.75f);
        Crucian += It->Species == EFishSpecies::Crucian;
        Pike += It->Species == EFishSpecies::Pike;
        Perch += It->Species == EFishSpecies::Perch;
        Roach += It->Species == EFishSpecies::Roach;
        TestTrue(TEXT("Fish starts in water"), LakeMap::IsWater(It->GetActorLocation().X, It->GetActorLocation().Y));
    }
    TestEqual(TEXT("Distinct fish IDs"), Ids.Num(), 10);
    TestEqual(TEXT("Pike"), Pike, 1);
    TestEqual(TEXT("Perch"), Perch, 1);
    TestEqual(TEXT("Roach"), Roach, 6);
    TestEqual(TEXT("Crucian"), Crucian, 2);
    TestEqual(TEXT("One local fry shoal for perch"), Life->FryShoals().Num(), 1);
    TestEqual(TEXT("Real minutes per day"), ALakeWeather::DayMinutes, 120.f);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeThreatResponseTest, "QuietLake.Ecosystem.ThreatResponse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeThreatResponseTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->StockEcosystemPrototype(/*bCampFixture=*/true);
    TSet<EFishSpecies> Tested;
    for (TActorIterator<ALakeFish> It(World); It; ++It)
    {
        if (Tested.Contains(It->Species)) continue;
        Tested.Add(It->Species);
        const FVector Start = It->GetActorLocation();
        const FVector Source = Start - It->GetActorForwardVector() * 60;
        It->Startle(Source, 1.f);
        bool Evaded = false;
        double Furthest = 0;
        for (int Frame = 0; Frame < 60; ++Frame)
        {
            It->Simulate(1.f / 30);
            Evaded |= FCString::Strcmp(It->PrototypePolicyStatus(), TEXT("ML: evade")) == 0;
            Furthest = FMath::Max(Furthest, FVector::Dist(Source, It->GetActorLocation()));
        }
        TestTrue(FString::Printf(TEXT("Species %d evaded, maximum distance %.0f cm (initially 60)"), int(It->Species), Furthest), Evaded && Furthest > 70);
    }
    TestEqual(TEXT("All four species exercised"), Tested.Num(), 4);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakePikeShoalTrial, "QuietLake.Ecosystem.PikeShoalTrial", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakePikeShoalTrial::RunTest(const FString& Parameters)
{
    for (bool WithPike : {false, true})
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        ALakeLife* Life = World->SpawnActor<ALakeLife>();
        Life->StockEcosystemPrototype(/*bCampFixture=*/true);
        TArray<ALakeFish*> Shoal;
        ALakeFish* Pike = nullptr;
        for (TActorIterator<ALakeFish> It(World); It; ++It)
        {
            if (It->Species == EFishSpecies::Roach) Shoal.Add(*It);
            else if (WithPike && It->Species == EFishSpecies::Pike) Pike = *It;
            else It->Destroy();
        }
        TestEqual(TEXT("Initial shoal"), Shoal.Num(), 6);
        TSet<int32> Evaded;
        float First = -1;
        for (int Frame = 0; Frame <= 300; ++Frame)
        {
            if (Frame > 0)
            {
                Life->Step(1.f / 30);
                if (Pike) Pike->Simulate(1.f / 30);
                for (ALakeFish* Fish : Shoal)
                {
                    if (!IsValid(Fish) || Fish->IsActorBeingDestroyed()) continue;
                    Fish->Simulate(1.f / 30);
                    if (FCString::Strcmp(Fish->PrototypePolicyStatus(), TEXT("ML: evade")) == 0)
                    {
                        Evaded.Add(Fish->PrototypeId);
                        if (First < 0) First = Frame / 30.f;
                    }
                }
            }
            if (Frame % 150 != 0) continue;
            double Sum = 0;
            int Pairs = 0, Alive = 0;
            for (ALakeFish* A : Shoal)
            {
                if (!IsValid(A) || A->IsActorBeingDestroyed()) continue;
                ++Alive;
                for (ALakeFish* B : Shoal)
                    if (IsValid(B) && !B->IsActorBeingDestroyed() && A->PrototypeId < B->PrototypeId)
                    {
                        Sum += FVector::Dist(A->GetActorLocation(), B->GetActorLocation());
                        ++Pairs;
                    }
            }
            AddInfo(FString::Printf(TEXT("Trial pike=%d time=%d alive=%d evaded=%d first_s=%.2f mean_pair_cm=%.1f"), WithPike, Frame / 30, Alive, Evaded.Num(), First, Sum / FMath::Max(1, Pairs)));
        }
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeEncounterCycle, "QuietLake.Ecosystem.EncounterCycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeEncounterCycle::RunTest(const FString& Parameters)
{
    const FVector2D Out = LakeMap::Spots()[0].Outward.GetSafeNormal();
    const FVector2D Along(-Out.Y, Out.X);
    for (EFishSpecies Kind : {EFishSpecies::Roach, EFishSpecies::Crucian, EFishSpecies::Perch})
    for (float Offset : {-180.f, 100.f, 300.f})
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        ALakeLife* Life = World->SpawnActor<ALakeLife>();
        Life->StockEcosystemPrototype(/*bCampFixture=*/true);
        TArray<ALakeFish*> Subjects;
        ALakeFish* Pike = nullptr;
        for (TActorIterator<ALakeFish> It(World); It; ++It)
        {
            if (It->Species == EFishSpecies::Pike) Pike = *It;
            else if (It->Species == Kind) Subjects.Add(*It);
            else It->Destroy();
        }
        if (!Pike || Subjects.IsEmpty()) { AddError(TEXT("Missing encounter actors")); World->DestroyWorld(false); return false; }
        // Stage the predator beyond sensory range until the baseline is recorded.
        Pike->SetActorLocation(Pike->GetActorLocation() - FVector(Out * 2500, 0));
        if (Kind == EFishSpecies::Perch && !Life->FryShoals().IsEmpty())
        {
            Subjects[0]->SetActorLocation(Life->FryLocation(0) + FVector(Along * 100, -25));
            Subjects[0]->TestHunger() = .85f;
        }
        int Start = -1, LastEscape = -1, FirstEscape = -1, FirstMeal = -1, NextHunt = -1;
        bool HuntingBefore = false, Interrupted = false, EnteredCover = false, FedOutside = false;
        TSet<int> Avoided, WasOutside, Sheltered;
        TMap<int32, int32> FirstPikeFrame;
        int Survivors = Subjects.Num();
        double InitialDistance = 0, FinalDistance = 0;
        const auto Meals = [&]{ return Kind == EFishSpecies::Roach ? Life->RoachMeals : Life->CrucianBenthosMeals; };
        double PreviousMeals = Meals(), MealsAtRemoval = 0;
        for (int Frame = 0; Frame < 8100; ++Frame)
        {
            const bool Hunts = Kind == EFishSpecies::Perch && Subjects[0]->TestTargetsFry();
            if (Start < 0 && Frame >= 60 && (Kind != EFishSpecies::Perch || Hunts || Frame >= 1800))
            {
                Start = Frame;
                HuntingBefore = Hunts;
                const FVector At = Subjects[0]->GetActorLocation() + FVector(Along * Offset, 0);
                Pike->SetActorLocation(At);
                for (ALakeFish* Fish : Subjects) InitialDistance += FVector::Dist(At, Fish->GetActorLocation()) / Subjects.Num();
            }
            const int Age = Frame - Start;
            if (Start >= 0 && Age == 450) Pike->Destroy();
            Life->Step(1.f/30);
            if (Start >= 0 && Age < 450) Pike->Simulate(1.f/30);
            Survivors = 0;
            for (ALakeFish* Fish : Subjects)
            {
                if (!IsValid(Fish) || Fish->IsActorBeingDestroyed()) continue;
                Fish->Simulate(1.f/30);
                if (!IsValid(Fish) || Fish->IsActorBeingDestroyed()) continue;
                ++Survivors;
                if (Start < 0) continue;
                const bool Evades = FCString::Strcmp(Fish->PrototypePolicyStatus(), TEXT("ML: evade")) == 0;
                const FVector P = Fish->GetActorLocation();
                const bool Covered = Life->CoverAt(P) >= .5f;
                if (Age < 450)
                {
                    if (Kind == EFishSpecies::Crucian)
                    {
                        const int32 Id = Fish->PrototypeId;
                        if (Fish->TestLastSensedPredatorId == Pike->PrototypeId && !FirstPikeFrame.Contains(Id))
                        {
                            FirstPikeFrame.Add(Id, Age);
                            AddInfo(FString::Printf(TEXT("Sensed offset=%.0f id=%d time=%.3f"), Offset, Id, Age / 30.));
                        }
                        if (Evades && !Avoided.Contains(Id))
                            AddInfo(FString::Printf(TEXT("Escape offset=%.0f id=%d time=%.3f"), Offset, Id, Age / 30.));
                    }
                    if (!Covered) WasOutside.Add(Fish->PrototypeId);
                    if (Evades)
                    {
                        Avoided.Add(Fish->PrototypeId);
                        if (FirstEscape < 0) FirstEscape = Age;
                        Interrupted |= HuntingBefore && !Fish->TestTargetsFry();
                    }
                    if (Covered && Avoided.Contains(Fish->PrototypeId) && WasOutside.Contains(Fish->PrototypeId))
                    { EnteredCover = true; Sheltered.Add(Fish->PrototypeId); }
                    if (Age == 449) FinalDistance += FVector::Dist(P, Pike->GetActorLocation());
                }
                else
                {
                    if (Evades) LastEscape = Age - 450;
                    if (Fish->TestTargetsFry() && NextHunt < 0) NextHunt = Age - 450;
                    if (Sheltered.Contains(Fish->PrototypeId) && !Covered && Meals() > PreviousMeals) FedOutside = true;
                }
                PreviousMeals = Meals();
            }
            if (Start >= 0 && Age == 449 && Survivors) FinalDistance /= Survivors;
            if (Start >= 0 && Age == 450) PreviousMeals = Meals();
            if (Start >= 0 && Age >= 450 && FirstMeal < 0 && Meals() > MealsAtRemoval) FirstMeal = Age - 450;
            if (Start >= 0 && Age == 449) MealsAtRemoval = Meals();
            if (Start >= 0 && Age >= 4050) break;
        }
        AddInfo(FString::Printf(TEXT("Cycle %s offset=%.0f first=%.2f evaded=%d alive=%d range_cm=%.0f->%.0f last_evade_after=%.2f meal_after=%.2f shelter=%d fed_outside=%d fry_before=%d interrupted=%d fry_after=%.2f"),
            *LakeFish::Info(Kind).Id, Offset, FirstEscape/30., Avoided.Num(), Survivors, InitialDistance, FinalDistance, LastEscape/30., FirstMeal/30., EnteredCover, FedOutside, HuntingBefore, Interrupted, NextHunt/30.));
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeBedTrial, "QuietLake.Ecosystem.CrucianBed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeBedTrial::RunTest(const FString& Parameters)
{
    const auto& Shore = LakeMap::Spots()[0];
    const FVector2D Out = Shore.Outward.GetSafeNormal(), Along(-Out.Y, Out.X);
    const FVector2D Centre = Shore.Location - Out * 820 + Along * 370;
    for (bool Inside : {false, true})
    for (float Side : {-1.f, 1.f})
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        ALakeLife* Life = World->SpawnActor<ALakeLife>();
        Life->StockEcosystemPrototype(/*bCampFixture=*/true);
        ALakeFish *Fish = nullptr, *Pike = nullptr;
        for (TActorIterator<ALakeFish> It(World); It; ++It)
            if (It->PrototypeId == 9) Fish = *It;
            else if (It->Species == EFishSpecies::Pike) Pike = *It;
            else It->Destroy();
        if (!Fish || !Pike) { AddError(TEXT("Missing fish")); World->DestroyWorld(false); return false; }
        const FVector2D Start = Centre + Out * (Inside ? 40 : 185);
        auto NearBottom = [](FVector2D P) { return FVector(P, ALakeFish::BottomZ(P.X,P.Y) + 20); };
        Fish->SetActorLocation(NearBottom(Start));
        Pike->SetActorLocation(NearBottom(Start + Out * 120 + Along * Side * 80));
        TestEqual(TEXT("Initial position relative to planted canopy"), Life->PlantedBedCover(Fish->GetActorLocation()) >= .5f, Inside);
        int First = -1, CoveredFrames = 0;
        double MealAfter = -1, InitialMeal = 0;
        bool Detected = false;
        for (int Frame = 0; Frame < 4050 && IsValid(Fish) && !Fish->IsActorBeingDestroyed(); ++Frame)
        {
            if (Frame == 450) { Pike->Destroy(); InitialMeal = Life->CrucianBenthosMeals; }
            Life->Step(1.f / 30);
            if (Frame < 450) Pike->Simulate(1.f / 30);
            if (!IsValid(Fish) || Fish->IsActorBeingDestroyed()) break;
            Fish->Simulate(1.f / 30);
            if (Frame < 450)
            {
                Detected |= Fish->TestLastSensedPredatorId == Pike->PrototypeId;
                if (Life->PlantedBedCover(Fish->GetActorLocation()) >= .5f)
                {
                    ++CoveredFrames;
                    if (First < 0) First = Frame;
                }
            }
            else if (MealAfter < 0 && Life->CrucianBenthosMeals > InitialMeal) MealAfter = (Frame - 450) / 30.;
        }
        AddInfo(FString::Printf(TEXT("BedTrial inside=%d side=%.0f detected=%d entered_s=%.2f cover_s=%.2f survived=%d meal_s=%.2f"), Inside, Side, Detected, First < 0 ? -1. : First/30., CoveredFrames/30., IsValid(Fish) && !Fish->IsActorBeingDestroyed(), MealAfter));
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeLearnedBait, "QuietLake.Ecosystem.LearnedBait", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeLearnedBait::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    FVector2D Facing;
    const FVector2D Shore = LakeMap::StartSpot(&Facing) + Facing * 90;
    const FRotator Rotation = FVector(Facing, 0).Rotation();
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Shore, 100), Rotation);
    for (float Distance : {500.f, 12.f})
    {
        FLakeFishingFixture::ImmersedRig(Fisher);
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Perch, .35f, Fisher->BaitLocation() - FVector(Facing, 0) * Distance, -1, 125);
        Fish->SetActorRotation(Rotation);
        Fish->PrototypeId = 2;
        Fish->TestSimTime() = 30; // Both observations start at a scheduled decision.
        Fish->SetActorLocation(Fish->GetActorLocation() + Fisher->BaitLocation() - Fish->MouthLocation() - FVector(Facing, 0) * Distance);
        Fish->TestHunger() = 1;
        bool Investigated = false, Took = false;
        for (int Frame = 0; Frame < (Distance > 100 ? 1 : 450); ++Frame)
        {
            Life->Step(1.f/30);
            Fish->Simulate(1.f/30);
            Investigated |= Fish->IsBusy();
            Took |= Fish->IsHoldingBait();
        }
        AddInfo(FString::Printf(TEXT("Bait trial distance=%.0f investigated=%d took=%d end_range=%.1f"), Distance, Investigated, Took, FVector::Dist(Fish->MouthLocation(), Fisher->BaitLocation())));
        if (Distance > 100) TestFalse(TEXT("Distant worm is not discovered at spawn"), Investigated);
        else TestTrue(TEXT("Hungry prototype perch reaches and takes a nearby worm"), Took);
        Fish->Destroy();
    }
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeRoachInspect, "QuietLake.Ecosystem.RoachInspect", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeRoachInspect::RunTest(const FString& Parameters)
{
    for (int Site : {10, 8}) // Shoal and backwater mouth, homes of the two roach groups.
    {
        const auto& Spot = LakeMap::Spots()[Site];
        const FVector Forward(-Spot.Outward, 0);
        const FVector FishHeading = FParse::Param(FCommandLine::Get(), TEXT("InspectFacingBank")) ? -Forward : Forward;
        const FVector At = FVector(Spot.Location, LakeFish::SurfaceZ) + Forward * 450;
        for (float Hunger : {.3f,.6f,.9f})
        for (float Angle : {-90.f,0.f,90.f})
        {
            int Seen = 0, Approach = 0, Sample = 0, Take = 0;
            for (int Seed = 125; Seed < 131; ++Seed)
            {
                UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
                GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
                ALakeLife* Life = World->SpawnActor<ALakeLife>();
                ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Spot.Location,100), Forward.Rotation());
                Fisher->FloatDepth = 115;
                FLakeFishingFixture::LandAt(Fisher, At, EBait::Worm);
                FLakeFishingFixture::FinishSettling(Fisher);
                ALakeFish* F = SpawnFish(World, EFishSpecies::Roach,.09f,Fisher->BaitLocation(),-1,Seed);
                F->SetActorRotation(FishHeading.Rotation());
                if (!FParse::Param(FCommandLine::Get(), TEXT("LegacyInspectPose")))
                    F->Setup(EFishSpecies::Roach,.09f,Fisher->BaitLocation(),-1,Seed);
                F->SetActorLocation(F->GetActorLocation() + Fisher->BaitLocation() - F->MouthLocation() - FishHeading.RotateAngleAxis(Angle,FVector::UpVector)*28);
                F->TestSimTime() = 30;
                F->PrototypeId = 3;
                F->TestHunger() = Hunger;
                bool Saw = false, Investigated = false, Tasted = false, Held = false;
                for (int Frame = 0; Frame < 900 && !Held; ++Frame)
                {
                    Life->Step(1.f/30);
                    F->TestObservedBait = -1;
                    F->TestTraceCandidates = Frame == 0 && Hunger > .8f && Seed == 125;
                    if (F->TestTraceCandidates)
                        UE_LOG(LogTemp, Display, TEXT("PatchStart site=%d angle=%.0f"), Site, Angle);
                    const FVector To = Fisher->BaitLocation() - F->MouthLocation();
                    const double Facing = FVector::DotProduct(F->GetActorForwardVector(), To.GetSafeNormal());
                    F->TestHookScore = F->TestWinningScore = -DBL_MAX;
                    F->Simulate(1.f/30);
                    if (Hunger > .8f && Seed == 125 && Frame < 150 && (Frame < 2 || F->TestObservedBait >= 0))
                        AddInfo(FString::Printf(TEXT("Pose site=%d angle=%.0f t=%.2f cm=%.1f facing=%.2f signal=%.3f hook=%.3f winner=%.3f action=%s food=%d"),
                            Site, Angle, Frame/30., To.Size(), Facing, F->TestObservedBait, F->TestHookScore == -DBL_MAX ? -999. : F->TestHookScore,
                            F->TestWinningScore == -DBL_MAX ? -999. : F->TestWinningScore, F->PrototypePolicyStatus(), int(F->GetFeedOn())));
                    Saw |= F->TestObservedBait > 0;
                    Investigated |= F->IsBusy();
                    Tasted |= F->IsNibbling();
                    Held |= F->IsHoldingBait();
                }
                Seen += Saw; Approach += Investigated; Sample += Tasted; Take += Held;
                GEngine->DestroyWorldContext(World);
                World->DestroyWorld(false);
            }
            AddInfo(FString::Printf(TEXT("Inspect site=%d hunger=%.1f angle=%.0f seen=%d approach=%d taste=%d take=%d"), Site, Hunger, Angle, Seen, Approach, Sample, Take));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSpeciesBites, "QuietLake.Ecosystem.SpeciesBites", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeSpeciesBites::RunTest(const FString& Parameters)
{
    for (EFishSpecies Species : {EFishSpecies::Pike, EFishSpecies::Perch, EFishSpecies::Roach, EFishSpecies::Crucian})
    for (float Appetite : {1.f, .55f})
    for (EBait History : {EBait::Count, EBait::Worm, EBait::Bread})
    for (EBait Bait : {EBait::Worm, EBait::Maggot, EBait::Bread})
    {
        if (Appetite < 1 && Species != EFishSpecies::Roach) continue;
        if (History != EBait::Count && Species != EFishSpecies::Crucian && Species != EFishSpecies::Roach) continue;
        int Approached = 0, Sampled = 0, Took = 0, Released = 0, Stolen = 0, Pending = 0;
        for (int Seed = 125; Seed < 131; ++Seed)
        {
            UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            ALakeLife* Life = World->SpawnActor<ALakeLife>();
            FVector2D Facing;
            const FVector2D Shore = LakeMap::StartSpot(&Facing) + Facing * 90;
            const FRotator Rotation = FVector(Facing, 0).Rotation();
            ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Shore, 100), Rotation);
            FLakeFishingFixture::ImmersedRig(Fisher, Bait);
            TestTrue(TEXT("Selected bait in water"), Fisher->HasBaitInWater() && Fisher->BaitOf() == Bait);
            float Kg = Species == EFishSpecies::Pike ? 2.4f : Species == EFishSpecies::Roach ? .09f : .24f;
            ALakeFish* Fish = SpawnFish(World, Species, Kg, Fisher->BaitLocation(), -1, Seed);
            Fish->SetActorRotation(Rotation);
            Fish->SetActorLocation(Fish->GetActorLocation() + Fisher->BaitLocation() - Fish->MouthLocation() - FVector(Facing, 0) * 12);
            Fish->PrototypeId = 1;
            if (History != EBait::Count) Fish->Eat(.45f, History);
            Fish->TestHunger() = Appetite; // Match hunger across feeding histories.
            Fish->TestSimTime() = 30;
            bool Approach = false, Sample = false, Take = false, Ended = false;
            for (int Frame = 0; Frame < 2700; ++Frame)
            {
                Life->Step(1.f / 30);
                Fish->Simulate(1.f / 30);
                Approach |= Fish->IsBusy();
                Sample |= Fish->IsNibbling();
                Take |= Fish->IsHoldingBait();
                if (Approach && !Fish->IsBusy())
                {
                    Released += Take;
                    Stolen += !Take && !Fisher->HasBaitInWater();
                    Ended = true;
                    break;
                }
            }
            Approached += Approach;
            Sampled += Sample;
            Took += Take;
            Pending += Approach && !Ended;
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
        AddInfo(FString::Printf(TEXT("BiteMatrix %s hunger=%.2f history=%d bait=%d approached=%d contact=%d took=%d released=%d stolen=%d pending=%d"), *LakeFish::Info(Species).Id, Appetite, int(History), int(Bait), Approached, Sampled, Took, Released, Stolen, Pending));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeCastAtPlants, "QuietLake.Ecosystem.CastAtPlants", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeCastAtPlants::RunTest(const FString& Parameters)
{
    const auto& Spot = LakeMap::Spots()[0];
    const FVector2D Out = Spot.Outward.GetSafeNormal(), Along(-Out.Y, Out.X);
    const FVector2D Edge = Spot.Location - Out * 650 + Along * 370;
    const FVector Target(Edge, LakeFish::SurfaceZ);
    constexpr float Dt = 1.f / 30;
    for (EFishSpecies Kind : {EFishSpecies::Crucian, EFishSpecies::Roach})
    for (int Scenario = 0; Scenario < (Kind == EFishSpecies::Roach ? 4 : 3); ++Scenario)
    for (int Run = 0; Run < 6; ++Run)
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        ALakeLife* Life = World->SpawnActor<ALakeLife>();
        Life->StockEcosystemPrototype(/*bCampFixture=*/true);
        TArray<ALakeFish*> Fish;
        for (TActorIterator<ALakeFish> It(World); It; ++It)
        {
            if (It->Species != Kind) { It->Destroy(); continue; }
            Fish.Add(*It);
            It->TestRandom().Initialize(1500 + Run * 20 + It->PrototypeId);
        }
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Spot.Location + Along * 370, 100), FVector(-Out, 0).Rotation());
        if (Scenario == 3) Life->Food().TestPlanktonScale = .25f;
        if (Scenario > 0) Life->ThrowGroundbait(Fisher->GetActorLocation(), Target, EBait::Worm);
        struct FTrace
        {
            double Minimum = DBL_MAX;
            float Peak = 0;
            int Checks = 0, Seen = 0, Positive = 0;
            FString AtMinimum, AtPeak;
            bool AteLastFrame = false;
            int MealChecks = 0, MealSeen = 0, MealPositive = 0, Paired = 0, HookBetter = 0, AboveLimit = 0, Behind = 0;
            double Vertical = 0, Gap = 0, MaxSignal = 0;
            int FirstSmell = -1, FirstChoice = -1, FirstMeal = -1, PositiveGround = 0;
            float MaxScent = 0;
            double MealDistance = DBL_MAX, ChoiceDistance = DBL_MAX;
        };
        struct FScores { int Count = 0; double Patch = 0, Natural = 0, Winner = 0, MinGap = DBL_MAX, MaxGap = -DBL_MAX; };
        TMap<FString, FScores> Scores;
        TMap<int, FTrace> Traces;
        int Approach = -1, Contact = -1, Take = -1;
        for (int Frame = -600; Frame < 5400; ++Frame)
        {
            if (Frame == 0)
            {
                const float Bottom = ALakeFish::BottomZ(Target.X, Target.Y);
                const bool BottomTrial = FParse::Param(FCommandLine::Get(), TEXT("BaitNearBottom"));
                if (Scenario > 0 && FParse::Param(FCommandLine::Get(), TEXT("QuarterGroundbait")) && !Life->Food().Groundbait().IsEmpty())
                {
                    // Remove three quarters of the remaining ball immediately before the cast.
                    Life->Food().EatGroundbait(0, Life->Food().Groundbait()[0].Balls * .75f / .7f, 1.f);
                }
                if (BottomTrial)
                    Fisher->FloatDepth = FMath::Clamp(FMath::RoundToFloat(LakeFish::SurfaceZ - Bottom - 8.f), ALakeCharacter::MinDepth, ALakeCharacter::MaxDepth);
                FLakeFishingFixture::LandAt(Fisher, Target, Scenario == 2 ? EBait::Bread : EBait::Worm);
                if (BottomTrial) TestTrue(TEXT("Hook eight centimetres above bed"), FMath::Abs(Fisher->BaitLocation().Z - Bottom - 8.) <= 1.);
                TestTrue(TEXT("Groundbait reached bed before cast"), Scenario == 0 || Life->Food().Groundbait().Num() > 0);
            }
            if (Frame == 3)
            {
                FLakeFishingFixture::FinishSettling(Fisher);
                TestTrue(TEXT("Landed bait is available"), Fisher->HasBaitInWater());
            }
            Life->Step(Dt);
            for (ALakeFish* F : Fish)
            {
                const double Distance = FVector::Dist(F->MouthLocation(), Fisher->BaitLocation());
                F->TestObservedBait = -1;
                const FVector ToHook = Fisher->BaitLocation() - F->MouthLocation();
                const bool Behind = Distance > 25 && FVector::DotProduct(F->GetActorForwardVector(), ToHook.GetSafeNormal()) < -.2f;
                F->TestHookScore = -DBL_MAX;
                F->TestBaitValue = -1;
                F->TestGroundScent = F->TestGroundValue = 0;
                F->TestPatchScore = F->TestNaturalScore = F->TestWinningScore = -DBL_MAX;
                auto Remaining = [&] { const auto& Spots = Life->Food().Groundbait(); return Spots.IsEmpty() ? 0.f : Spots[0].Balls; };
                const float Before = Remaining();
                F->Simulate(Dt);
                FTrace& Ground = Traces.FindOrAdd(F->PrototypeId);
                // Include decisions that switch away from groundbait on this very frame.
                if (Ground.AteLastFrame && Frame >= 3 && F->TestObservedBait >= 0)
                {
                    ++Ground.MealChecks;
                    Ground.Vertical += ToHook.Z;
                    Ground.AboveLimit += FMath::Abs(ToHook.Z) > 55;
                    Ground.Behind += Behind;
                    Ground.MealSeen += F->TestObservedBait > 0;
                    Ground.MealPositive += F->TestBaitValue > 0;
                    Ground.MaxSignal = FMath::Max(Ground.MaxSignal, double(F->TestObservedBait));
                    if (F->TestHookScore > -DBL_MAX && F->TestPatchScore > -DBL_MAX)
                    {
                        if (Scenario == 1 && FParse::Param(FCommandLine::Get(), TEXT("TracePatchGap")))
                            UE_LOG(LogTemp, Display, TEXT("PatchGap %s %d %d %.5f %.3f %.6f %.6f %.6f"),
                                *LakeFish::Info(Kind).Id, Run, F->PrototypeId, Before, Distance, F->TestPatchScore, F->TestHookScore, F->TestPatchBonus);
                        ++Ground.Paired;
                        Ground.Gap += F->TestPatchScore - F->TestHookScore;
                        Ground.HookBetter += F->TestHookScore > F->TestPatchScore;
                    }
                }
                Ground.AteLastFrame = Before > Remaining();
                const int Elapsed = Frame + 600;
                if (F->TestGroundScent > 0 && Ground.FirstSmell < 0) Ground.FirstSmell = Elapsed;
                Ground.MaxScent = FMath::Max(Ground.MaxScent, F->TestGroundScent);
                Ground.PositiveGround += F->TestGroundValue > 0;
                if (F->TestPatchScore > -DBL_MAX && F->TestNaturalScore > -DBL_MAX)
                {
                    const FString Key = FString::Printf(TEXT("%d/%d/%d"), int(F->GetGoal()), int(F->GetFeedOn()), int(F->TestNaturalKind));
                    FScores& S = Scores.FindOrAdd(Key);
                    ++S.Count;
                    S.Patch += F->TestPatchScore;
                    S.Natural += F->TestNaturalScore;
                    S.Winner += F->TestWinningScore;
                    const double Gap = F->TestWinningScore - F->TestPatchScore;
                    S.MinGap = FMath::Min(S.MinGap, Gap);
                    S.MaxGap = FMath::Max(S.MaxGap, Gap);
                }
                if (F->GetFeedOn() == EFishFood::Groundbait)
                {
                    if (Ground.FirstChoice < 0) Ground.FirstChoice = Elapsed;
                    if (Fisher->HasBaitInWater()) Ground.ChoiceDistance = FMath::Min(Ground.ChoiceDistance, Distance);
                }
                if (Before > Remaining())
                {
                    if (Ground.FirstMeal < 0) Ground.FirstMeal = Elapsed;
                    if (Fisher->HasBaitInWater()) Ground.MealDistance = FMath::Min(Ground.MealDistance, Distance);
                }
                if (Frame < 0) continue;
                if (Fisher->HasBaitInWater())
                {
                    FTrace& Trace = Traces.FindOrAdd(F->PrototypeId);
                    auto Goal = [&] { return FString::Printf(TEXT("%s/%d"), F->PrototypePolicyStatus(), int(F->GetFeedOn())); };
                    if (Distance < Trace.Minimum) { Trace.Minimum = Distance; Trace.AtMinimum = Goal(); }
                    Trace.Checks += F->TestObservedBait >= 0;
                    Trace.Seen += F->TestObservedBait > 0;
                    Trace.Positive += F->TestBaitValue > 0;
                    if (F->TestObservedBait > Trace.Peak) { Trace.Peak = F->TestObservedBait; Trace.AtPeak = Goal(); }
                }
                if (Approach < 0 && F->IsBusy()) Approach = Frame;
                if (Contact < 0 && F->IsNibbling()) Contact = Frame;
                if (Take < 0 && F->IsHoldingBait()) Take = Frame;
            }
            if (Take >= 0) break;
        }
        for (const auto& Entry : Scores)
        {
            const FScores& S = Entry.Value;
            AddInfo(FString::Printf(TEXT("GroundScore %s %d %d %s n=%d patch=%.6f natural=%.6f winner=%.6f min=%.6f max=%.6f"),
                *LakeFish::Info(Kind).Id, Scenario, Run, *Entry.Key, S.Count, S.Patch, S.Natural, S.Winner, S.MinGap, S.MaxGap));
        }
        for (const auto& Entry : Traces)
        {
            const FTrace& T = Entry.Value;
            AddInfo(FString::Printf(TEXT("MealHook %s %d %d %d %d %d %d %d %d %d %d %.4f %.4f %.4f"),
                *LakeFish::Info(Kind).Id, Scenario, Run, Entry.Key, T.MealChecks, T.MealSeen, T.MealPositive, T.Paired, T.HookBetter,
                T.AboveLimit, T.Behind, T.Vertical, T.Gap, T.MaxSignal));
            AddInfo(FString::Printf(TEXT("GroundTrace %s %d %d %d smell=%d positive=%d choice=%d meal=%d peak=%.4f target_cm=%.1f meal_cm=%.1f"),
                *LakeFish::Info(Kind).Id, Scenario, Run, Entry.Key, T.FirstSmell, T.PositiveGround, T.FirstChoice, T.FirstMeal, T.MaxScent,
                T.ChoiceDistance == DBL_MAX ? -1 : T.ChoiceDistance, T.MealDistance == DBL_MAX ? -1 : T.MealDistance));
            AddInfo(FString::Printf(TEXT("HookTrace %s %d %d %d cm=%.2f checks=%d seen=%d positive=%d peak=%.5f close=[%s] best=[%s]"),
                *LakeFish::Info(Kind).Id, Scenario, Run, Entry.Key, T.Minimum, T.Checks, T.Seen, T.Positive, T.Peak, *T.AtMinimum, *T.AtPeak));
        }
        AddInfo(FString::Printf(TEXT("PlantCast %s scenario=%d run=%d approach=%d contact=%d take=%d"), *LakeFish::Info(Kind).Id, Scenario, Run, Approach, Contact, Take));
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeUnfedShoreTrial, "QuietLake.Ecosystem.UnfedShore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeUnfedShoreTrial::RunTest(const FString& Parameters)
{
    for (const auto& Spot : LakeMap::Spots())
    {
        const auto& Pier = LakeMap::Pier();
        const FVector2D Dir = Spot.bBoards ? -Spot.Outward : Pier.Dir;
        const FVector2D Bank = Spot.bBoards ? Spot.Location : Pier.Root + Pier.Dir * Pier.Length;
        for (float Reach : {300.f, 450.f, 600.f})
        {
            const FVector2D XY = Bank + Dir * Reach;
            const FVector Target(XY, LakeFish::SurfaceZ);
            UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            ALakeLife* Life = World->SpawnActor<ALakeLife>();
            Life->StockEcosystemPrototype();
            const bool Trace = FString(Spot.Name) == TEXT("\u041e\u0442\u043c\u0435\u043b\u044c");
            const bool RemovePike = Trace && FParse::Param(FCommandLine::Get(), TEXT("ShoalWithoutPike"));
            TArray<ALakeFish*> Fish;
            for (TActorIterator<ALakeFish> It(World); It; ++It)
            {
                if (RemovePike && It->Species == EFishSpecies::Pike) It->Destroy();
                else Fish.Add(*It);
            }
            ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank,100), FVector(Dir,0).Rotation());
            const float Depth = LakeFish::SurfaceZ - ALakeFish::BottomZ(XY.X,XY.Y);
            Fisher->FloatDepth = FMath::Clamp(FMath::RoundToFloat(Depth - 8.f), ALakeCharacter::MinDepth, ALakeCharacter::MaxDepth);
            TArray<float> FeedingDepths;
            int Approach = -1, Contact = -1, Take = -1, Holder = 0;
            double Nearest = DBL_MAX;
            struct FObservation {
                double Minimum = DBL_MAX, DecisionDistance = DBL_MAX;
                float Signal = -1, Value = -1;
                int Seen = 0, Positive = 0, Bolting = 0;
                FString CloseAction, DecisionAction, VisibleAction;
            };
            TMap<int, FObservation> Observations;
            for (int Frame = -600; Frame < 5400 && Take < 0; ++Frame)
            {
                if (Frame == 0)
                {
                    if (Trace)
                    {
                        FeedingDepths.Sort();
                        const float Median = FeedingDepths.IsEmpty() ? -1 : FeedingDepths[FeedingDepths.Num()/2];
                        if (FParse::Param(FCommandLine::Get(), TEXT("UseShoalFeedingDepth")) && Median >= 0)
                            Fisher->FloatDepth = FMath::Clamp(FMath::RoundToFloat(Median), ALakeCharacter::MinDepth, ALakeCharacter::MaxDepth);
                        AddInfo(FString::Printf(TEXT("LayerTrial reach=%.0f samples=%d median=%.1f set=%.1f"), Reach, FeedingDepths.Num(), Median, Fisher->FloatDepth));
                    }
                    const bool Maggot = Trace && Reach == 450.f && FParse::Param(FCommandLine::Get(), TEXT("ShoalMaggot"));
                    FLakeFishingFixture::LandAt(Fisher, Target, Maggot ? EBait::Maggot : EBait::Worm);
                    if (Maggot) TestTrue(TEXT("Maggot on comparison rig"), Fisher->BaitOf() == EBait::Maggot);
                }
                if (Frame == 3)
                {
                    FLakeFishingFixture::FinishSettling(Fisher);
                    TestTrue(FString::Printf(TEXT("Rig in water at %s %.0f"), Spot.Name, Reach), Fisher->HasBaitInWater());
                }
                Life->Step(1.f/30);
                for (ALakeFish* F : Fish)
                {
                    if (!IsValid(F) || F->IsActorBeingDestroyed()) continue;
                    const double Before = FVector::Dist(F->MouthLocation(), Fisher->BaitLocation());
                    if (Trace) F->TestObservedBait = F->TestBaitValue = -1;
                    F->TestExplainBait = Trace && Reach == 450.f && Frame >= 3 && F->Species == EFishSpecies::Roach;
                    const double MealsBefore = Life->RoachMeals;
                    F->Simulate(1.f/30);
                    if (Trace && Frame < 0 && F->GetSchool() == 1 && F->Species == EFishSpecies::Roach && Life->RoachMeals > MealsBefore)
                        FeedingDepths.Add(LakeFish::SurfaceZ - F->MouthLocation().Z);
                    if (Frame < 3) continue;
                    Nearest = FMath::Min(Nearest,FVector::Dist(F->MouthLocation(), Fisher->BaitLocation()));
                    if (Trace)
                    {
                        auto Action = [&] { return FString::Printf(TEXT("%s/%d"), F->PrototypePolicyStatus(), int(F->GetFeedOn())); };
                        FObservation& O = Observations.FindOrAdd(F->PrototypeId);
                        const double After = FVector::Dist(F->MouthLocation(), Fisher->BaitLocation());
                        if (After < O.Minimum) { O.Minimum = After; O.CloseAction = Action(); }
                        if (F->TestObservedBait >= 0 && Before < O.DecisionDistance)
                        {
                            O.DecisionDistance = Before; O.Signal = F->TestObservedBait; O.Value = F->TestBaitValue; O.DecisionAction = Action();
                        }
                        if (F->TestExplainBait && F->TestObservedBait > 0 && F->TestBaitValue != -1)
                            AddInfo(FString::Printf(TEXT("BaitInputs frame=%d id=%d cm=%.1f %s"), Frame, F->PrototypeId, Before, *F->TestBaitInputs));
                        if (F->TestObservedBait > 0) { ++O.Seen; O.VisibleAction = Action(); }
                        O.Positive += F->TestBaitValue > 0;
                        O.Bolting += Before <= 160 && FCString::Strcmp(F->PrototypePolicyStatus(), TEXT("ML: evade")) == 0;
                    }
                    if (Approach < 0 && F->IsBusy()) Approach = Frame;
                    if (Contact < 0 && F->IsNibbling()) Contact = Frame;
                    if (F->IsHoldingBait()) { Take = Frame; Holder = F->PrototypeId; }
                }
            }
            for (const auto& E : Observations)
            {
                const auto& O = E.Value;
                if (O.Minimum > 2000) continue;
                AddInfo(FString::Printf(TEXT("ShoalTrace reach=%.0f fish=%d cm=%.1f action=[%s] decision_cm=%.1f signal=%.5f value=%.4f decision=[%s] seen=%d positive=%d lastseen=[%s] evading_frames=%d"),
                    Reach, E.Key, O.Minimum, *O.CloseAction, O.DecisionDistance == DBL_MAX ? -1 : O.DecisionDistance, O.Signal, O.Value,
                    *O.DecisionAction, O.Seen, O.Positive, *O.VisibleAction, O.Bolting));
            }
            AddInfo(FString::Printf(TEXT("UnfedCast site=[%s] reach=%.0f depth=%.0f distance=%.1f approach=%d contact=%d take=%d fish=%d"),
                Spot.Name, Reach, Depth, Nearest, Approach, Contact, Take, Holder));
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFullPondTrial, "QuietLake.Ecosystem.FullPond", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFullPondTrial::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->StockFullPrototype();
    const auto Spots = LakeMap::Spots();
    TArray<ALakeFish*> Fish;
    TArray<FVector> Start;
    struct FSample { int Initial = 0, Alive = 0, Moved = 0, Feed = 0; TSet<int> Sites; };
    TArray<FSample> Samples;
    Samples.SetNum(LakeFish::NumSpecies());
    double Kg = 0;
    for (TActorIterator<ALakeFish> It(World); It; ++It)
    {
        Fish.Add(*It);
        Start.Add(It->GetActorLocation());
        ++Samples[int(It->Species)].Initial;
        Kg += It->WeightKg;
    }
    const FBox2D Bounds = LakeMap::LakeBounds();
    double Hectares = 0;
    for (double X = Bounds.Min.X + 100; X < Bounds.Max.X; X += 200)
        for (double Y = Bounds.Min.Y + 100; Y < Bounds.Max.Y; Y += 200)
            if (LakeMap::IsWater(X,Y)) Hectares += .0004;
    AddInfo(FString::Printf(TEXT("FullStock fish=%d frySchools=%d hectares=%.2f kg=%.2f"), Fish.Num(), Life->FryShoals().Num(), Hectares, Kg));
    TArray<ALakeCharacter*> Anglers;
    for (int Site = 0; Site < Spots.Num(); Site += 4)
    {
        const auto& Spot = Spots[Site];
        const auto& Pier = LakeMap::Pier();
        const FVector2D Dir = Spot.bBoards ? -Spot.Outward : Pier.Dir;
        const FVector2D Bank = Spot.bBoards ? Spot.Location : Pier.Root + Pier.Dir * Pier.Length;
        const FVector Target(Bank + Dir * 450, LakeFish::SurfaceZ);
        auto* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank,100), FVector(Dir,0).Rotation());
        Fisher->FloatDepth = FMath::Clamp(float(Target.Z - ALakeFish::BottomZ(Target.X,Target.Y) - 8), ALakeCharacter::MinDepth, ALakeCharacter::MaxDepth);
        FLakeFishingFixture::LandAt(Fisher,Target,EBait::Worm);
        FLakeFishingFixture::FinishSettling(Fisher);
        TestTrue(TEXT("Worm in water"), Fisher->HasBaitInWater());
        Anglers.Add(Fisher);
    }
    TSet<int> Fed;
    TArray<TSet<int>> Contacts;
    Contacts.SetNum(Anglers.Num());
    for (int Frame = 0; Frame < 600; ++Frame)
    {
        Life->Step(.1f);
        for (ALakeFish* F : Fish)
        {
            if (!IsValid(F) || F->IsActorBeingDestroyed()) continue;
            F->Simulate(.1f);
            if (F->GetGoal() == EFishGoal::Feed) Fed.Add(F->PrototypeId);
            if (F->IsNibbling() || F->IsHoldingBait())
                for (int J = 0; J < Anglers.Num(); ++J)
                    if (FVector::Dist(F->MouthLocation(),Anglers[J]->BaitLocation()) < 100)
                        Contacts[J].Add(F->PrototypeId);
        }
    }
    for (int I = 0; I < Fish.Num(); ++I)
    {
        ALakeFish* F = Fish[I];
        if (!IsValid(F) || F->IsActorBeingDestroyed()) continue;
        auto& S = Samples[int(F->Species)];
        ++S.Alive;
        S.Feed += Fed.Contains(F->PrototypeId);
        S.Moved += FVector::Dist(F->GetActorLocation(), Start[I]) > 100;
        const FVector2D At(F->GetActorLocation());
        int Best = 0;
        for (int J = 1; J < Spots.Num(); ++J)
            if ((At-Spots[J].Location).SizeSquared() < (At-Spots[Best].Location).SizeSquared()) Best = J;
        S.Sites.Add(Best);
    }
    for (int I = 0; I < Samples.Num(); ++I)
    {
        const auto& S = Samples[I];
        const auto Species = EFishSpecies(I);
        TestEqual(LakeFish::Info(Species).Id, S.Initial, Life->StockCount(Species));
        TestTrue(TEXT("Species moves and occupies multiple sectors"), S.Moved > 0 && S.Sites.Num() > 1);
        AddInfo(FString::Printf(TEXT("FullSpecies %s initial=%d alive=%d moved=%d feedGoal=%d sectors=%d"), *LakeFish::Info(Species).Id, S.Initial,S.Alive,S.Moved,S.Feed,S.Sites.Num()));
    }
    for (int I = 0; I < Anglers.Num(); ++I)
        AddInfo(FString::Printf(TEXT("FullCast %s contacts=%d"), Spots[I*4].Name,Contacts[I].Num()));
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FLakeRoachDepthTrial, "QuietLake.Ecosystem.RoachDepthTrial", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FLakeRoachDepthTrial::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (int Seed = 1; Seed <= 2; ++Seed)
        for (const TCHAR* Layer : {TEXT("Bottom"), TEXT("Mid")})
        {
            const FString Key = FString::Printf(TEXT("%d%s"), Seed, Layer);
            Names.Add(Key);
            Commands.Add(Key);
        }
}

bool FLakeRoachDepthTrial::RunTest(const FString& Key)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->StockFullPrototype(20261026 + FCString::Atoi(*Key));
    struct FCohort { TSet<int32> Near, Seen, HungryCalm, Approach, Nibble, Take; };
    struct FCast
    {
        ALakeCharacter* Fisher;
        FString Site, FirstSpecies;
        FCohort Group[3];
        float Depth, Water, FirstNibble = -1, FirstTake = -1;
        bool Done = false;
    };
    TArray<FCast> Casts;
    for (const auto& Spot : LakeMap::Spots())
    {
        const FString Site(Spot.Name);
        if (Site != TEXT("Лагерь") && Site != TEXT("Омут") && Site != TEXT("Луговая бровка")) continue;
        const auto& Pier = LakeMap::Pier();
        const FVector2D Dir = Spot.bBoards ? -Spot.Outward : Pier.Dir;
        const FVector2D Bank = Spot.bBoards ? Spot.Location : Pier.Root + Pier.Dir * Pier.Length;
        const FVector Target(Bank + Dir * 450, LakeFish::SurfaceZ);
        auto& C = Casts.AddDefaulted_GetRef();
        C.Site = Site;
        C.Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank,100), FVector(Dir,0).Rotation());
        C.Water = Target.Z - ALakeFish::BottomZ(Target.X,Target.Y);
        C.Fisher->FloatDepth = FMath::Clamp(Key.EndsWith(TEXT("Mid")) ? C.Water * .5f : C.Water - 8.f,
            ALakeCharacter::MinDepth, ALakeCharacter::MaxDepth);
        FLakeFishingFixture::LandAt(C.Fisher,Target,EBait::Worm);
        FLakeFishingFixture::FinishSettling(C.Fisher);
        C.Depth = LakeFish::SurfaceZ - C.Fisher->BaitLocation().Z;
        TestTrue(TEXT("Worm in water"), C.Fisher->BaitOf() == EBait::Worm && C.Fisher->HasBaitInWater());
    }
    TestEqual(TEXT("Three stations"), Casts.Num(), 3);
    TArray<ALakeFish*> Fish;
    int Roach = 0;
    for (TActorIterator<ALakeFish> It(World); It; ++It)
    {
        Fish.Add(*It);
        Roach += It->Species == EFishSpecies::Roach;
    }
    TestEqual(TEXT("Dense prototype roach population"), Roach, 1000);
    constexpr float Dt = .1f;
    for (int Frame = 1; Frame <= 1800; ++Frame)
    {
        World->Tick(LEVELTICK_TimeOnly, Dt);
        Life->Step(Dt);
        for (ALakeFish* F : Fish)
        {
            if (!IsValid(F) || F->IsActorBeingDestroyed()) continue;
            F->TestSeenAngler.Reset();
            F->Simulate(Dt);
            const bool IsRoach = F->Species == EFishSpecies::Roach;
            for (auto& C : Casts)
            {
                if (C.Done) continue;
                const bool Target = F->TestTargetAngler().Get() == C.Fisher;
                const bool Nibble = Target && F->IsNibbling(), Take = Target && F->IsHoldingBait();
                if (IsRoach)
                {
                    auto& G = C.Group[F->WeightKg <= .1f ? 0 : F->WeightKg <= .35f ? 1 : 2];
                    const int Id = F->PrototypeId;
                    if (C.Fisher->HasBaitInWater() && FVector::DistSquared(F->MouthLocation(), C.Fisher->BaitLocation()) <= 160.*160.) G.Near.Add(Id);
                    if (F->TestSeenAngler.Get() == C.Fisher)
                    {
                        G.Seen.Add(Id);
                        if (F->GetHunger() >= .5f && F->FearNow() <= .1f) G.HungryCalm.Add(Id);
                    }
                    if (Target && F->IsBusy()) G.Approach.Add(Id);
                    if (Nibble) G.Nibble.Add(Id);
                    if (Take) G.Take.Add(Id);
                }
                if ((Nibble || Take) && C.FirstNibble < 0)
                {
                    C.FirstNibble = Frame * Dt;
                    C.FirstSpecies = LakeFish::Info(F->Species).Id;
                }
                if (Take) { C.FirstTake = Frame * Dt; C.Done = true; }
            }
        }
        for (auto& C : Casts)
            if (!C.Fisher->HasBaitInWater()) C.Done = true;
    }
    for (const auto& C : Casts)
    {
        AddInfo(FString::Printf(TEXT("DepthCast %s %s depth=%.0f water=%.0f nibble=%.1f species=%s take=%.1f"), *Key,*C.Site,C.Depth,C.Water,C.FirstNibble,*C.FirstSpecies,C.FirstTake));
        for (int Size = 0; Size < 3; ++Size)
        {
            const auto& G = C.Group[Size];
            AddInfo(FString::Printf(TEXT("DepthFish %s %s size=%d near=%d seen=%d hungryCalm=%d approach=%d nibble=%d take=%d"), *Key,*C.Site,Size,G.Near.Num(),G.Seen.Num(),G.HungryCalm.Num(),G.Approach.Num(),G.Nibble.Num(),G.Take.Num()));
        }
    }
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}


#endif
