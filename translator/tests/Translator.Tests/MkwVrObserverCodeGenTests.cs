using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public sealed class MkwVrObserverCodeGenTests
{
    [Fact]
    public void EmitsOnlyWhenProjectConfiguresAnObserver()
    {
        var function = new IrFunction(
            "race_scene_enter",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[] { new IrReturn(null) })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>());
        var signature = new FunctionAbiClassification("race_scene_enter", ValueRepresentation.Void);
        var ssa = new SsaTransformer().Convert(function);

        var observed = new CxxLinearCodeGenerator().EmitWithFacts(
            0x80553C50u, ssa, signature, types,
            entryObserverHeader: "vr/mkw_vr_instrumentation.h",
            entryObserverSymbol: "MkwVRObserveTranslatedFunctionEntry");
        var ordinary = new CxxLinearCodeGenerator().Emit(0x80553C50u, ssa, signature, types);

        Assert.Contains("#include \"vr/mkw_vr_instrumentation.h\"", observed.Code);
        Assert.Contains(
            "MkwVRObserveTranslatedFunctionEntry(0x80553C50u, static_cast<const CpuContext*>(ctx));",
            observed.Code);
        Assert.True(observed.GuestAbiContract.HasFullSynchronizationFence);
        Assert.Equal(uint.MaxValue, observed.GuestAbiContract.GprReadBeforeWriteMask);
        Assert.Equal(0u, observed.GuestAbiContract.GprPossibleWriteMask);
        Assert.Contains("fence=1", observed.GuestAbiMarker);
        Assert.DoesNotContain("MkwVRObserveTranslatedFunctionEntry", ordinary);
    }
}
