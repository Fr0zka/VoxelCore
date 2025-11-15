#include "VoxelStreamingSourceComponent.h"
#include "VoxelWorld.h"
#include "EngineUtils.h"

UVoxelStreamingSourceComponent::UVoxelStreamingSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics; // Update before VoxelWorld tick

	// Default settings
	ViewDistanceChunks = 32;
	ViewDistanceChunksZ = 5;
	CollisionRadius = 2;
	AORadius = 3;
	bUseFrustumPriority = true;
	FrustumHorizontalFOV = 120.0f;
	bFrustumPriorityVertical = false;
	FrustumVerticalFOV = 100.0f;
	PriorityWeight = 1.0f;
	bIsPlayerSource = false;
	bDiskShapedLoading = true;
	bEnableOcclusionCulling = false;
	OcclusionMinDistance = 8;
	bIsActive = false;

	// Initialize cached data
	CachedLocation = FVector::ZeroVector;
	CachedRotation = FRotator::ZeroRotator;
	CachedForward = FVector::ForwardVector;
	CachedForwardXY = FVector::ForwardVector;
}

void UVoxelStreamingSourceComponent::BeginPlay()
{
	Super::BeginPlay();

	bIsActive = true;

	// Cache initial transform
	if (AActor* Owner = GetOwner())
	{
		CachedLocation = Owner->GetActorLocation();
		CachedRotation = Owner->GetActorRotation();
		CachedForward = CachedRotation.Vector();
		CachedForwardXY = FVector(CachedForward.X, CachedForward.Y, 0.0f).GetSafeNormal();
	}

	UE_LOG(LogTemp, Log, TEXT("[VoxelStreamingSource] '%s' registered (Player=%d, Priority=%.2f, ViewDist=%d)"),
		*GetOwner()->GetName(), bIsPlayerSource ? 1 : 0, PriorityWeight, ViewDistanceChunks);
}

void UVoxelStreamingSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bIsActive = false;

	UE_LOG(LogTemp, Log, TEXT("[VoxelStreamingSource] '%s' unregistered"),
		GetOwner() ? *GetOwner()->GetName() : TEXT("Unknown"));

	Super::EndPlay(EndPlayReason);
}

void UVoxelStreamingSourceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsActive) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	// OPTIMIZATION: Cache transform data for VoxelWorld to avoid repeated GetActorLocation() calls
	CachedLocation = Owner->GetActorLocation();
	CachedRotation = Owner->GetActorRotation();
	CachedForward = CachedRotation.Vector();
	CachedForwardXY = FVector(CachedForward.X, CachedForward.Y, 0.0f).GetSafeNormal();
}

void UVoxelStreamingSourceComponent::SetEnabled(bool bEnabled)
{
	bIsActive = bEnabled;

	if (bEnabled)
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelStreamingSource] '%s' enabled"), *GetOwner()->GetName());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelStreamingSource] '%s' disabled"), *GetOwner()->GetName());
	}
}
