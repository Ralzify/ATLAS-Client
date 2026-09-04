#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Pure-text transaction planner for accepted DefaultGame.ini revisions.
//
// This component has no scheduler, reflection, or UObject dependency. Callers
// first feed the exact accepted DefaultGame payload through Unreal normally.
// For a DeltaNative plan they may then temporarily install DeltaIni, invoke one
// native PatchAssetsFromIniFiles pass, and install RestoreIni. RestoreIni is a
// synthetic AssetHotfix section containing every active directive from the
// accepted revision; unrelated DefaultGame sections are never replayed.
//
// BaselineRestorations maps the canonical identity returned by
// TryGetDirectiveIdentity to one exact, previously accepted RowUpdate line.
// The line is parsed again and must resolve to the same identity. Missing or
// invalid restoration input is reported and omitted from a safe forward-only
// delta; replaying the full accepted INI cannot restore an absent directive.
namespace AtlasAssetHotfixRevisionPlanner
{
	using FBaselineRestorationMap =
		std::map<std::string, std::string>;
	using FKnownResolvableIdentitySet = std::set<std::string>;
	using FKnownUnresolvableIdentitySet = std::set<std::string>;

	enum class ERevisionMode : std::uint8_t
	{
		ByteIdentical,
		NoAssetChange,
		FullNative,
		DeltaNative
	};

	struct FRevisionPlan
	{
		ERevisionMode Mode = ERevisionMode::FullNative;
		std::uint64_t Token = 0;
		std::string DeltaIni;
		std::string RestoreIni;
		std::size_t AddedOrChanged = 0;
		// One independent unchanged current directive per active table when one
		// exists, preserving native tracking even if a changed row is rejected.
		std::size_t RetentionReapplied = 0;
		std::size_t Restored = 0;
		std::size_t MissingBaselines = 0;
		std::vector<std::string> MissingBaselineIdentities;
		// Exact current directives whose identities were not present in the
		// committed revision. On the first parsed revision, this contains all
		// active directives; the caller decides whether sampling is pristine.
		std::vector<std::string> BaselineCaptureCandidates;
		// Every individually valid current RowUpdate, even when an unsupported
		// sibling makes the revision full-native-only. Used only for bounded
		// post-native resolvability probes; never as restoration data.
		std::vector<std::string> ActiveDirectives;
		// A safe full-replay recovery is represented as DeltaNative so validated
		// baselines for removed rows can be applied in the same single native pass.
		// Callers must treat every capture candidate as untrusted for this plan.
		bool FullReplayRecovery = false;
		bool AcceptedRevisionSafe = false;
		bool BaselineRestorationsSafe = true;
	};

	// Parses one complete +CurveTable/+DataTable RowUpdate line and returns its
	// canonical identity. This is the only identity construction callers should
	// use when populating FBaselineRestorationMap.
	bool TryGetDirectiveIdentity(
		const std::string& ExactDirectiveLine,
		std::string& OutIdentity) noexcept;

	// Serialized caller only. The byte-identical fast path compares against the
	// last committed exact payload and performs no parsing or delta construction.
	// NoAssetChange means only non-operative payload text changed: callers should
	// skip PatchAssetsFromIniFiles but still commit Token. ForceFull is intended
	// for the first observed pass and lifecycle transitions.
	bool PrepareRevision(
		const std::string& ExactAcceptedDefaultGame,
		const FBaselineRestorationMap& BaselineRestorations,
		const FKnownResolvableIdentitySet& KnownResolvableIdentities,
		const FKnownUnresolvableIdentitySet& KnownUnresolvableIdentities,
		bool ForceFull,
		FRevisionPlan& OutPlan) noexcept;

	// Compatibility overload for pure-text callers that have no native
	// resolvability evidence.
	bool PrepareRevision(
		const std::string& ExactAcceptedDefaultGame,
		const FBaselineRestorationMap& BaselineRestorations,
		bool ForceFull,
		FRevisionPlan& OutPlan) noexcept;

	// Commit only after the selected native operation returned and, for a delta,
	// RestoreIni was accepted. NoAssetChange may be committed without a native
	// asset pass. Abort leaves the committed active set unchanged.
	void CommitRevision(std::uint64_t Token) noexcept;
	void AbortRevision(std::uint64_t Token) noexcept;
}
