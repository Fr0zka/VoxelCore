// VoxelChunk.h
// Identifiant léger de chunk.
//
// Rôle: dans un monde density-only (pas de blocs), le chunk n'a plus rien
// à stocker — la densité est évaluée à la volée par le générateur à partir
// des coordonnées monde. On garde un struct fin pour:
//   - Servir de clé/valeur dans les collections de AVoxelWorld (Chunks, FChunkResult)
//   - Fournir l'helper GetWorldPosition() au mesher
//   - Laisser une place si on veut cacher des infos par chunk plus tard
//     (index de strate, LOD courant, etc.)

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"
#include "VoxelChunk.generated.h"

USTRUCT(BlueprintType)
struct FVoxelChunk
{
    GENERATED_BODY()

    // Coordonnée de chunk dans la grille mondiale (peut être négative).
    FIntVector ChunkCoord = FIntVector::ZeroValue;

    FVoxelChunk() = default;
    explicit FVoxelChunk(const FIntVector& InCoord) : ChunkCoord(InCoord) {}

    // Coin (0,0,0) du chunk en espace monde (cm).
    FVector GetWorldPosition() const
    {
        return ChunkToWorldPos(ChunkCoord);
    }
};
