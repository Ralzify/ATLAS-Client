#pragma once
#include "../../pch.h"

struct FObjectReplicationBridgeFilterConfig
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FObjectReplicationBridgeFilterConfig);

    DEFINE_STRUCT_PROP(ClassName, FName);
    DEFINE_STRUCT_PROP(DynamicFilterName, FName);
};
class UObjectReplicationBridgeConfig : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UObjectReplicationBridgeConfig);

    DEFINE_PROP(FilterConfigs, TArray<FObjectReplicationBridgeFilterConfig>);
};

class Client
{
public:
    static void Init();
    static void SetConsoleCaptureEnabled(bool enabled);
    static bool IsConsoleCaptureReady();
    static bool HasReceivedServerCommandList();
    static uint64_t GetConsoleSessionGeneration();
};
