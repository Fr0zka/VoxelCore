// VoxelForgeStrateParamCoverageTest.cpp
// Le garde-fou de la X-macro : tout champ de FStrateGenerationParams DOIT être listé dans
// VF_STRATE_PARAM_FIELDS, sinon il est silencieusement remis à sa valeur par défaut à chaque
// fusion de frontière de strate.
//
// The X-macro guard: every field of FStrateGenerationParams MUST be listed in
// VF_STRATE_PARAM_FIELDS, or it is silently reset to its default on every strate-boundary blend.
//
// POURQUOI CE TEST EXISTE / WHY THIS TEST EXISTS
// ---------------------------------------------
// `FStrateGenerationParams::Lerp` ne fusionne PAS la structure : il fusionne la LISTE écrite à la
// main dans `VF_STRATE_PARAM_FIELDS`. Ajouter un champ à la structure sans l'ajouter à la liste
// compile parfaitement, se teste vert partout, et produit un bug qui ne ressemble pas à un bug :
// le champ prend sa valeur par défaut dans toute bande de transition Gradient/Interleaved. Le
// terrain change de forme à la frontière et nulle part ailleurs. CODEMAP §3.8 avertit du piège en
// prose depuis toujours ; la prose ne casse pas la CI.
//
// `Lerp` does not blend the STRUCT — it blends the hand-written list in `VF_STRATE_PARAM_FIELDS`.
// Adding a field to the struct without adding it to the list compiles fine, tests green, and
// produces a bug that does not look like one: the field silently takes its default inside every
// Gradient/Interleaved transition band. CODEMAP §3.8 has warned about this in prose forever; prose
// does not fail CI.
//
// L'ENJEU AUGMENTE / THE STAKES ARE RISING
// Le composeur de mondes (voir COMPOSER-NOTES.md) prévoit d'INVENTER des jeux de paramètres en
// mélangeant des vecteurs connus-bons via ce même `Lerp`. Un champ manquant ne casserait alors plus
// seulement les frontières : il serait invariant dans TOUTES les strates inventées, et l'auteur
// n'aurait aucun moyen de le remarquer. Ce test transforme l'avertissement en échec.
//
// The world composer (see COMPOSER-NOTES.md) intends to INVENT parameter sets by blending
// known-good vectors through this same `Lerp`. A missing field would then not merely break
// boundaries — it would be constant across EVERY invented strate, with nothing to notice it by.
// This test turns the warning into a failure.
//
// COMMENT / HOW
// La X-macro est développée une TROISIÈME fois (après LERP et SNAP) : en liste de noms. On compare
// cette liste à la réflexion UObject de la structure. Aucun monde n'est construit — c'est un test
// de forme, pas de comportement, donc il est instantané et ne dépend d'aucune fixture.
//
// The X-macro is expanded a THIRD way (after LERP and SNAP): into a list of names. That list is
// compared against the struct's UObject reflection. No world is built — this is a shape test, not a
// behaviour test, so it is instant and depends on no fixture.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "VoxelStrateTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeStrateParamCoverageTest,
    "VoxelForge.Determinism.StrateParamBlendCoverage",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    /** Troisième expansion de la X-macro : les noms qu'elle couvre.
     *  Third expansion of the X-macro: the names it covers. */
#define VF_PARAM_NAME(Name) TEXT(#Name),
    const TCHAR* const GBlendedFieldNames[] =
    {
        VF_STRATE_PARAM_FIELDS(VF_PARAM_NAME, VF_PARAM_NAME)
    };
#undef VF_PARAM_NAME

    /**
     * Champs délibérément NON fusionnés. Vide aujourd'hui, et c'est le message : à ce jour, chaque
     * champ réfléchi de la structure est couvert par la X-macro, sans exception.
     * Pour en exempter un, l'ajouter ICI avec la raison — l'exemption doit être un choix visible,
     * pas un oubli.
     *
     * Deliberately UN-blended fields. Empty today, and that is the point: as of now every reflected
     * field of the struct is covered by the X-macro, with no exceptions. To exempt one, add it HERE
     * with the reason — an exemption must be a visible decision, never an omission.
     */
    const TCHAR* const GExemptFieldNames[] =
    {
        nullptr  // sentinelle : garde le tableau non vide / sentinel: keeps the array non-empty
    };
}

bool FVoxelForgeStrateParamCoverageTest::RunTest(const FString& Parameters)
{
    const UScriptStruct* Struct = FStrateGenerationParams::StaticStruct();
    if (!Struct)
    {
        AddError(TEXT("FStrateGenerationParams::StaticStruct() returned null — reflection is unavailable."));
        return false;
    }

    TSet<FString> Covered;
    for (const TCHAR* const Name : GBlendedFieldNames)
    {
        if (Name)
        {
            Covered.Add(FString(Name));
        }
    }

    TSet<FString> Exempt;
    for (const TCHAR* const Name : GExemptFieldNames)
    {
        if (Name)
        {
            Exempt.Add(FString(Name));
        }
    }

    int32 NumChecked = 0;
    TArray<FString> Missing;

    for (TFieldIterator<FProperty> It(Struct); It; ++It)
    {
        const FProperty* Prop = *It;
        if (!Prop)
        {
            continue;
        }

        const FString PropName = Prop->GetName();
        ++NumChecked;

        if (Covered.Contains(PropName) || Exempt.Contains(PropName))
        {
            continue;
        }

        // Le type C++ est dans le message : si un jour un champ non scalaire apparaît, le
        // correctif ("fusionner comment ?") doit être évident depuis le seul rapport de test.
        // The C++ type is in the message: if a non-scalar field ever appears, the fix ("blend it
        // how?") must be obvious from the test report alone.
        Missing.Add(FString::Printf(TEXT("%s (%s)"), *PropName, *Prop->GetCPPType()));
    }

    if (Missing.Num() > 0)
    {
        AddError(FString::Printf(
            TEXT("%d field(s) of FStrateGenerationParams are absent from VF_STRATE_PARAM_FIELDS, so ")
            TEXT("FStrateGenerationParams::Lerp silently resets them to default in every transition ")
            TEXT("band (and in every strate the world composer invents): %s. ")
            TEXT("Fix: add each to VF_STRATE_PARAM_FIELDS (LERPF for continuous, SNAPF for discrete), ")
            TEXT("or to GExemptFieldNames in this test with a written reason."),
            Missing.Num(),
            *FString::Join(Missing, TEXT(", "))));
        return false;
    }

    // Un test qui ne regarde rien passe aussi. On vérifie donc qu'il a bien regardé.
    // A test that inspects nothing also passes. So check that it actually inspected something.
    if (NumChecked == 0)
    {
        AddError(TEXT("Walked FStrateGenerationParams and found no reflected properties at all — ")
                 TEXT("the test is not testing anything. Check the struct is still USTRUCT()."));
        return false;
    }

    AddInfo(FString::Printf(
        TEXT("%d reflected field(s) checked, %d covered by VF_STRATE_PARAM_FIELDS, %d exempt."),
        NumChecked, Covered.Num(), Exempt.Num()));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
