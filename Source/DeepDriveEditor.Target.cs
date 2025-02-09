// 

using UnrealBuildTool;
using System.Collections.Generic;
using System;
using System.Text;
using System.IO;
using System.Reflection;

public class DeepDriveEditorTarget : TargetRules
{
	public DeepDriveEditorTarget(TargetInfo Target)
	{
		Type = TargetType.Editor;
    }

    //
    // TargetRules interface.
    //

    public override void SetupBinaries(
		TargetInfo Target,
		ref List<UEBuildBinaryConfiguration> OutBuildBinaryConfigurations,
		ref List<string> OutExtraModuleNames
		)
	{
		OutExtraModuleNames.AddRange( new string[] { "DeepDrive" } );
	}
}
