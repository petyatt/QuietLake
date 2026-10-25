using UnrealBuildTool;
public class QuietLake : ModuleRules
{
    public QuietLake(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ProceduralMeshComponent", "DynamicWind", "AnimationCore", "CableComponent", "UMG", "Slate", "SlateCore" });
    }
}
