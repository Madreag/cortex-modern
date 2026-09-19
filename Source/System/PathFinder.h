#pragma once

#include "Box.h"
#include "System/MicroPather/micropather.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace micropather;

namespace RTE {

	class Scene;
	class Material;

	/// Information required to make an async pathing request.
	struct PathRequest {
		bool complete = false;
		int status = MicroPather::NO_SOLUTION;
		std::list<Vector> path;
		float pathLength = 0.0f;
		float totalCost = 0.0f;
		Vector startPos;
		Vector targetPos;
		uint64_t horizonGeneration = 0; //!< Overlay generation pinned when the request is launched.
	};

	using PathCompleteCallback = std::function<void(std::shared_ptr<volatile PathRequest>)>;

	/// Material pixels captured at a terrain box's origin tick for an off-thread horizon raycast.
	/// Indexed in the unwrapped box frame Capture wrote: local (x,y) is world (originX+x, originY+y).
	struct HorizonTerrainPatch {
		int originX = 0;
		int originY = 0;
		int width = 0;
		int height = 0;
		int sceneWidth = 0;
		int sceneHeight = 0;
		bool wrapsX = false;
		bool wrapsY = false;
		std::vector<unsigned char> pixels;

		unsigned char Sample(int x, int y) const;
		bool ContainsUnwrapped(int x, int y) const;
	};

	/// One path node as it stood at the origin tick. The worker never reads the live grid.
	struct HorizonNodeSnapshot {
		int nodeId = -1;
		Vector pos;
		bool navigable = true;
		std::array<const Material*, 8> materials{};
		std::array<int, 8> neighborIds{};
		std::array<Vector, 8> neighborPos{};
	};

	/// Overlay cells and in-flight jobs captured with a terrain fence.
	struct HorizonFenceState {
		struct Cell {
			std::array<const Material*, 8> materials{};
			bool navigable = true;
			uint64_t generation = 0;
		};
		std::unordered_map<int, Cell> overlay;
		std::deque<std::shared_ptr<void>> jobs;
		uint64_t generation = 1;
	};

	/// Contains everything related to a PathNode on the path grid used by PathFinder.
	struct PathNode {

		static constexpr int c_MaxAdjacentNodeCount = 8; //!< The maximum number of adjacent PathNodes to any given PathNode. Thusly, also the number of directions for PathNodes to be in.

		Vector Pos; //!< Absolute position of the center of this PathNode in the scene.

		bool m_Navigable; //!< Whether this node can be navigated through.

		/// Pointers to all adjacent PathNodes, in clockwise order with top first. These are not owned, and may be 0 if adjacent to non-wrapping scene border.
		std::array<PathNode*, c_MaxAdjacentNodeCount> AdjacentNodes;
		PathNode*& Up = AdjacentNodes[0];
		PathNode*& UpRight = AdjacentNodes[1];
		PathNode*& Right = AdjacentNodes[2];
		PathNode*& RightDown = AdjacentNodes[3];
		PathNode*& Down = AdjacentNodes[4];
		PathNode*& DownLeft = AdjacentNodes[5];
		PathNode*& Left = AdjacentNodes[6];
		PathNode*& LeftUp = AdjacentNodes[7];

		/// The strongest material between us and our adjacent PathNodes, in clockwise order with top first.
		std::array<const Material*, c_MaxAdjacentNodeCount> AdjacentNodeBlockingMaterials;
		const Material*& UpMaterial = AdjacentNodeBlockingMaterials[0];
		const Material*& UpRightMaterial = AdjacentNodeBlockingMaterials[1];
		const Material*& RightMaterial = AdjacentNodeBlockingMaterials[2];
		const Material*& RightDownMaterial = AdjacentNodeBlockingMaterials[3];
		const Material*& DownMaterial = AdjacentNodeBlockingMaterials[4];
		const Material*& DownLeftMaterial = AdjacentNodeBlockingMaterials[5];
		const Material*& LeftMaterial = AdjacentNodeBlockingMaterials[6];
		const Material*& LeftUpMaterial = AdjacentNodeBlockingMaterials[7];

		/// Constructor method used to instantiate a PathNode object in system memory and make it ready for use.
		/// @param pos Absolute position of the center of the PathNode in the scene.
		explicit PathNode(const Vector& pos);
	};

	/// A class encapsulating and implementing the MicroPather A* pathfinding library.
	class PathFinder : public Graph {
		friend struct ContractAudit;

	public:
		PathFinder() { Clear(); }
#pragma region Creation
		/// Constructor method used to instantiate a PathFinder object.
		/// @param nodeDimension The width and height in scene pixels that of each PathNode should represent.
		/// @param allocate The block size that the PathNode cache is allocated from. Should be about a fourth of the total number of PathNodes.
		PathFinder(int nodeDimension);

		/// Makes the PathFinder object ready for use.
		/// @param nodeDimension The width and height in scene pixels that of each PathNode should represent.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int Create(int nodeDimension);
#pragma endregion

#pragma region Destruction
		/// Destructor method used to clean up a PathFinder object before deletion.
		~PathFinder() override;

		/// Destroys and resets (through Clear()) this PathFinder object.
		void Destroy();

		/// Resets the entire PathFinder object to the default settings or values.
		void Reset() { Clear(); }
#pragma endregion

#pragma region PathFinding
		/// Calculates and returns the least difficult path between two points on the current scene.
		/// This is synchronous, and will block the current thread!
		/// @param start Start positions on the scene to find the path between.
		/// @param end End positions on the scene to find the path between.
		/// @param pathResult A list which will be filled out with waypoints between the start and end.
		/// @param totalCostResult The total minimum difficulty cost calculated between the two points on the scene.
		/// @param jumpHeight How high, in metres, the search can jump vertically.
		/// @param digStrength What material strength the search is capable of digging through.
		/// @return Success or failure, expressed as SOLVED, NO_SOLUTION, or START_END_SAME.
		int CalculatePath(Vector start, Vector end, std::list<Vector>& pathResult, float& totalCostResult, float jumpHeight, float digStrength, bool committedHorizon = false);

		/// Calculates and returns the least difficult path between two points on the current scene.
		/// This is asynchronous and thus will not block the current thread.
		/// @param start Start positions on the scene to find the path between.
		/// @param end End positions on the scene to find the path between.
		/// @param jumpHeight How high, in metres, the search can jump vertically.
		/// @param digStrength What material strength the search is capable of digging through.
		/// @param callback The callback function to be run when the path calculation is completed.
		/// @return A shared pointer to the volatile PathRequest to be used to track whether the asynchronous path calculation has been completed, and check its results.
		std::shared_ptr<volatile PathRequest> CalculatePathAsync(Vector start, Vector end, float jumpHeight, float digStrength, PathCompleteCallback callback = nullptr, bool committedHorizon = false);

		/// Queues a committed-horizon recompute for terrain that landed at originTick.
		void QueueHorizonUpdate(uint64_t originTick, uint16_t horizonTicks, const std::vector<Box>& boxes, const std::vector<HorizonTerrainPatch>& patches = {}, const std::vector<HorizonNodeSnapshot>& nodes = {});

		/// Copies live cells into the overlay at Note time, before any live rewrite of the box.
		void PinHorizonFromLive(const Box& box);

		/// Snapshots terrain material pixels for the worker raycast.
		void CaptureHorizonPatch(const Box& box, HorizonTerrainPatch& patch) const;

		/// Snapshots node materials, navigable flags and neighbour ids at Note time.
		void CaptureHorizonNodeSnapshots(const Box& box, std::vector<HorizonNodeSnapshot>& nodes) const;

		/// Applies every horizon job whose commit tick is due. A job not ready at T+H stalls the sim tick until it is.
		void CommitHorizonThrough(uint64_t nowTick);

		void CaptureHorizonFence(HorizonFenceState& out) const;
		void RestoreHorizonFence(const HorizonFenceState& in);

		static constexpr int64_t c_HorizonWaitCapUs = 250000;
		uint64_t BeginCommittedHorizonRead();
		void EndCommittedHorizonRead(uint64_t generation);

		void SetHorizonWorkerDelayMs(int milliseconds) { m_HorizonWorkerDelayMs = milliseconds; }
		void TestSetHorizonWorkerLate(bool late) { m_HorizonWorkerLate = late; }
		void TestHoldHorizonWorker() { m_HorizonWorkerHold.store(true); }
		void TestReleaseHorizonWorker() { m_HorizonWorkerHold.store(false); }
		void TestSetWraps(bool wrapX, bool wrapY);
		int64_t LastHorizonWaitUs() const { return m_LastHorizonWaitUs; }
		int64_t LastHorizonReaderWaitUs() const { return m_LastHorizonReaderWaitUs; }
		uint64_t TestHorizonGeneration() const { return m_HorizonGeneration.load(); }
		size_t TestHorizonOverlayCount() const;
		bool TestHorizonWrapRayMatches(const Vector& start, const Vector& end, const HorizonTerrainPatch& patch) const;
		std::array<const Material*, 8> TestViewMaterials(int nodeId, uint64_t generation) const;
		void TestComputeHorizon(const std::vector<HorizonNodeSnapshot>& nodes, const std::vector<HorizonTerrainPatch>& patches, std::vector<std::array<const Material*, 8>>& materials, std::vector<char>& navigable) const;

		static int64_t HorizonWaitCount();
		static int64_t HorizonWaitP99Us();
		static void ResetHorizonWaitStats();
		static void WriteHorizonWaitReport();
		static int RunHorizonGridSelfTest();

		void TestInstallGrid(int width, int height, int nodeDimension, const Material* fill);
		void TestSetNodeMaterials(int nodeId, const std::array<const Material*, 8>& materials);
		void TestHoldPathingRequest() { ++m_CurrentPathingRequests; }
		void TestReleasePathingRequest() { --m_CurrentPathingRequests; }
		void TestApplyLiveUpdate(int nodeId, const std::array<const Material*, 8>& materials);
		void QueueHorizonDelta(uint64_t originTick, uint16_t horizonTicks, int nodeId, const std::array<const Material*, 8>& materials, bool navigable = true);
		void TestQueueHorizonCompute(uint64_t originTick, uint16_t horizonTicks, int nodeId);
		int TestNodeIdAt(int x, int y) const { return ConvertCoordsToNodeId(x, y); }

		// <summary>
		/// Returns how many pathfinding requests are currently active.
		/// @return How many pathfinding requests are currently active.
		int GetCurrentPathingRequests() const { return m_CurrentPathingRequests.load(); }

		/// Waits until submitted path requests and their callbacks have finished.
		void WaitForPathingRequests() const;

		/// Preserve the partially updated node graph at a completed checkpoint boundary.
		std::string SaveCheckpoint() const;
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false);

		/// Recalculates all the costs between all the PathNodes by tracing lines in the material layer and summing all the material strengths for each encountered pixel. Also resets the pather itself.
		void RecalculateAllCosts();

		/// Recalculates the costs between all the PathNodes touching a deque of specific rectangular areas (which will be wrapped). Also resets the pather itself, if necessary.
		/// @param boxList The deque of Boxes representing the updated areas.
		/// @param nodeUpdateLimit The maximum number of PathNodes we'll try to update this frame. True PathNode update count can be higher if we received a big box, as we always do at least 1 box.
		/// @return The set of PathNode ids that were updated.
		std::vector<int> RecalculateAreaCosts(std::deque<Box>& boxList, size_t nodeUpdateLimit);

		/// Updates a set of PathNodes, adjusting their transitions.
		/// This does NOT update the pather, which is required if PathNode costs changed.
		/// @param nodeVec The set of PathNode IDs to update.
		/// @return Whether any PathNode costs changed.
		bool UpdateNodeList(const std::vector<int>& nodeVec);

		/// Implementation of the abstract interface of Graph.
		/// Gets the least possible cost to get from PathNode A to B, if it all was air.
		/// @param startState Pointer to PathNode to start from. OWNERSHIP IS NOT TRANSFERRED!
		/// @param endState PathNode to end up at. OWNERSHIP IS NOT TRANSFERRED!
		/// @return The cost of the absolutely fastest possible way between the two points, as if traveled through air all the way.
		float LeastCostEstimate(void* startState, void* endState) override;

		/// Implementation of the abstract interface of Graph.
		/// Gets the cost to go to any adjacent PathNode of the one passed in.
		/// @param state Pointer to PathNode to get to cost of all adjacents for. OWNERSHIP IS NOT TRANSFERRED!
		/// @param adjacentList An empty vector which will be filled out with all the valid PathNodes adjacent to the one passed in. If at non-wrapping edge of seam, those non existent PathNodes won't be added.
		void AdjacentCost(void* state, std::vector<micropather::StateCost>* adjacentList) override;

		/// Returns whether two position represent the same path nodes.
		/// @param pos1 First coordinates to compare.
		/// @param pos2 Second coordinates to compare.
		/// @return Whether both coordinates represent the same path node.
		bool PositionsAreTheSamePathNode(const Vector& pos1, const Vector& pos2) const;

		/// Marks a box as being navigable or not.
		/// @param box The Box of which all PathNodes that should have their navigable status changed.
		/// @param navigable Whether or not the nodes in this box should be navigable.
		void MarkBoxNavigable(Box box, bool navigable);

		/// Marks a box as being navigable or not.
		/// @param box The Box of which all PathNodes that should have their navigable status changed.
		/// @param navigable Whether or not the nodes in this box should be navigable.
		void MarkAllNodesNavigable(bool navigable);
#pragma endregion

#pragma region Misc
		/// Implementation of the abstract interface of Graph. This function is only used in DEBUG mode - it dumps output to stdout.
		/// Since void* aren't really human readable, this will print out some concise info without an ending newline.
		/// @param state The state to print out info about.
		void PrintStateInfo(void* state) override {}

		/// Draws a debug rendering for this pathfinder to a BITMAP of choice.
		/// @param targetBitmap A pointer to a BITMAP to draw on.
		/// @param targetPos The offset into the scene where the target bitmap's upper left corner is located.
		void DebugRender(BITMAP* targetBitmap, const Vector& targetPos = Vector()) const;
#pragma endregion

	private:
		static constexpr float c_NodeCostChangeEpsilon = 5.0F; //!< The minimum change in a PathNodes's cost for the pathfinder to recognize a change and reset itself. This is so minor changes (e.g. blood particles) don't force constant pathfinder resets.

		MicroPather* m_Pather; //!< The actual pathing object that does the pathfinding work. Owned.
		std::vector<PathNode> m_NodeGrid; //!< The array of PathNodes representing the grid on the scene.
		unsigned int m_NodeDimension; //!< The width and height of each PathNode, in pixels on the scene.
		Vector m_Offset;
		int m_GridWidth; //!< The width of the pathing grid, in PathNodes.
		int m_GridHeight; //!< The height of the pathing grid, in PathNodes.
		bool m_WrapsX; //!< Whether the pathing grid wraps on the X axis.
		bool m_WrapsY; //!< Whether the pathing grid wraps on the Y axis.
		std::atomic<int> m_CurrentPathingRequests{0}; //!< The number of queued or running path requests.

		struct HorizonNode {
			std::array<const Material*, 8> committedMaterials{};
			bool committedNavigable = true;
			uint64_t generation = 0;
		};
		struct HorizonJob {
			uint64_t originTick = 0;
			uint64_t commitTick = 0;
			std::vector<int> nodeIds;
			std::vector<HorizonNodeSnapshot> nodes;
			std::vector<std::array<const Material*, 8>> materials;
			std::vector<char> navigable;
			std::vector<HorizonTerrainPatch> patches;
			int delayMs = 0;
			bool late = false;
			std::atomic<bool> hold{false};
			std::atomic<bool> ready{false};
		};
		mutable std::unordered_map<int, HorizonNode> m_HorizonNodes;
		std::deque<std::shared_ptr<HorizonJob>> m_HorizonJobs;
		mutable std::mutex m_HorizonMutex;
		std::condition_variable m_HorizonApplyCv;
		bool m_HorizonApplyPending = false;
		std::atomic<int> m_CommittedHorizonReaders{0};
		std::unordered_map<uint64_t, int> m_HorizonReaderCounts;
		std::atomic<uint64_t> m_HorizonGeneration{1};
		bool m_SelfTestGrid = false;
		int m_HorizonWorkerDelayMs = 0;
		bool m_HorizonWorkerLate = false;
		std::atomic<bool> m_HorizonWorkerHold{false};
		int64_t m_LastHorizonWaitUs = 0;
		int64_t m_LastHorizonReaderWaitUs = 0;
		int64_t m_LastHorizonExpired = 0;

		struct NodeCostView {
			std::array<const Material*, 8> materials{};
			bool navigable = true;
		};
		NodeCostView ViewNode(const PathNode* node) const;
		static const Material* StrongestMaterialAlongPatch(const Vector& start, const Vector& end, const std::vector<HorizonTerrainPatch>& patches);
		static void ComputeHorizonMaterialsFromSnapshots(const std::vector<HorizonNodeSnapshot>& nodes, const std::vector<HorizonTerrainPatch>& patches, std::vector<std::array<const Material*, 8>>& materials, std::vector<char>& navigable);
		HorizonNodeSnapshot SnapshotNode(int nodeId) const;
		void LaunchHorizonWorker(const std::shared_ptr<HorizonJob>& job);
		void ApplyHorizonJob(const HorizonJob& job, uint64_t generation);
		void ClearHorizonState();
		bool HasOlderHorizonReaders(uint64_t applyGeneration) const;

		/// Gets the pather for this thread. Lazily-initialized for each new thread that needs a pather.
		/// @return The pather for this thread.
		MicroPather* GetPather();

		/// Calculates a path within an already registered request.
		int CalculatePathImpl(Vector start, Vector end, std::list<Vector>& pathResult, float& totalCostResult, float jumpHeight, float digStrength);

#pragma region Path Cost Updates
		/// Helper function for getting the strongest material we need to path though between PathNodes.
		/// @param start Origin point.
		/// @param end Destination point.
		/// @return The strongest material.
		const Material* StrongestMaterialAlongLine(const Vector& start, const Vector& end) const;

		/// Helper function for updating all the values of cost edges going out from a specific PathNodes.
		/// This does NOT update the pather, which is required before solving more paths after calling this.
		/// @param node The PathNode to update all costs of. It's safe to pass nullptr here. OWNERSHIP IS NOT TRANSFERRED!
		/// @return Whether the PathNodes costs changed.
		bool UpdateNodeCosts(PathNode* node) const;

		/// Helper function for getting the PathNode ids in a Box.
		/// @param box The Box of which all PathNodes it touches should be returned.
		/// @return A list of the PathNode ids inside the box.
		std::vector<int> GetNodeIdsInBox(Box box) const;

		/// Every PathNode a live area update writes: the nodes in the box plus the neighbours UpdateNodeList mirrors into.
		std::vector<int> GetHorizonNodeIdsInBox(const Box& box) const;

		/// Helper function to determine if a node is on solid fround.
		/// @param node The node we're checking.
		/// @return Whether the node is on solid ground.
		bool NodeIsOnSolidGround(const PathNode& node) const;

		/// Gets the cost for transitioning through this Material.
		/// @param material The Material to get the transition cost for.
		/// @return The transition cost for the Material.
		float GetMaterialTransitionCost(const Material& material) const;

		/// Gets the average cost for all transitions out of this PathNode, ignoring infinities/unpathable transitions.
		/// @param node The PathNode to get the average transition cost for.
		/// @return The average transition cost.
		float GetNodeAverageTransitionCost(const PathNode& node) const;
#pragma endregion

		/// Gets the PathNode at the given coordinates.
		/// @param x The X coordinate, in PathNodes.
		/// @param y The Y coordinate, in PathNodes.
		/// @return The PathNode at the given coordinates.
		PathNode* GetPathNodeAtGridCoords(int x, int y);

		/// Gets the PathNode id at the given coordinates.
		/// @param x The X coordinate, in PathNodes.
		/// @param y The Y coordinate, in PathNodes.
		/// @return The PathNode id at the given coordinates.
		int ConvertCoordsToNodeId(int x, int y) const;

		/// Clears all the member variables of this PathFinder, effectively resetting the members of this abstraction level only.
		void Clear();

		// Disallow the use of some implicit methods.
		PathFinder(const PathFinder& reference) = delete;
		PathFinder& operator=(const PathFinder& rhs) = delete;
	};
} // namespace RTE
