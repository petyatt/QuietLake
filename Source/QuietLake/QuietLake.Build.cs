using UnrealBuildTool;
public class QuietLake : ModuleRules
{
    public QuietLake(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Keep file-local helpers isolated in clean and adaptive builds.
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ProceduralMeshComponent", "DynamicWind", "AnimationCore", "CableComponent", "UMG", "Slate", "SlateCore", "Json", "JsonUtilities", "PCG" });
    }
}
