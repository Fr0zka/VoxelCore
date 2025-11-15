#include "VoxelStructs.h"

// ============================================================================
// STATIC MEMBER INITIALIZATION FOR FCategoryBitset
// ============================================================================
// Initialize lookup table for fast bit extraction (10-20% faster Get())
uint8 FCategoryBitset::BitExtractLUT[256 * 8];
bool FCategoryBitset::bLUTInitialized = false;
