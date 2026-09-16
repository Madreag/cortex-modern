#include "PathFinder.h"

#include "Material.h"
#include "Scene.h"
#include "SceneMan.h"
#include "SLTerrain.h"
#include "ThreadMan.h"
#include "CheckpointArchive.h"
#include "FaultInjection.h"
#include "System.h"
#include "Controller.h"
#include "Actor.h"
#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "ScenarioRunner.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <execution>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>

using namespace RTE;

namespace {
	struct PathingRequestScope {
		std::atomic<int>& requests;
		~PathingRequestScope() { --requests; }
	};
}

// One pathfinder per thread, lazily initialized. Shouldn't access this directly, use GetPather() instead.
struct MicroPatherWrapper {
	MicroPatherWrapper() {
		m_Instance = nullptr;
	}

	~MicroPatherWrapper() {
		delete m_Instance;
	}

	MicroPather* m_Instance;
};

thread_local MicroPatherWrapper s_Pather;

// How high the given agent can jump / jetpack vertically, in metres
thread_local float s_JumpHeight = 0.0F;

// How high the given agent can jump / jetpack vertically, in nodes
thread_local int s_JumpHeightVertical = 0;
thread_local int s_JumpHeightDiagonal = 0;

// What material strength the search is capable of digging through.
// Needs to be thread-local because of how it's passed around, unfortunately it doesn't seem we can give userdata for a path agent in MicroPather.
// TODO: Enhance MicroPather to add that capability (or write our own pather)!
thread_local float s_DigStrength = 0.0F;

thread_local bool s_ReadCommittedHorizon = false;

namespace {
	constexpr size_t c_HorizonWaitRing = 256;

	struct HorizonWaitStats {
		std::mutex mutex;
		uint64_t count = 0;
		int64_t lastUs = 0;
		std::array<int64_t, c_HorizonWaitRing> ring{};
		size_t ringCount = 0;
		size_t ringNext = 0;
	};

	HorizonWaitStats& HorizonStats() {
		static HorizonWaitStats stats;
		return stats;
	}

	void RecordHorizonWait(int64_t waitUs) {
		auto& stats = HorizonStats();
		std::lock_guard lock(stats.mutex);
		stats.count += 1;
		stats.lastUs = waitUs;
		stats.ring[stats.ringNext] = waitUs;
		stats.ringNext = (stats.ringNext + 1) % c_HorizonWaitRing;
		if (stats.ringCount < c_HorizonWaitRing) {
			stats.ringCount += 1;
		}
	}

	int64_t HorizonP99Locked(const HorizonWaitStats& stats) {
		if (stats.ringCount == 0) {
			return 0;
		}
		std::vector<int64_t> sample(stats.ring.begin(), stats.ring.begin() + static_cast<std::ptrdiff_t>(stats.ringCount));
		std::sort(sample.begin(), sample.end());
		const size_t index = static_cast<size_t>(std::ceil(0.99 * static_cast<double>(sample.size()))) - 1;
		return sample[std::min(index, sample.size() - 1)];
	}

	struct HorizonReadScope {
		HorizonReadScope(PathFinder* finder, bool enable) :
		    m_Finder(finder),
		    m_Enable(enable) {
			if (m_Enable && m_Finder) {
				s_ReadCommittedHorizon = true;
				m_Finder->BeginCommittedHorizonRead();
			}
		}
		~HorizonReadScope() {
			if (m_Enable && m_Finder) {
				m_Finder->EndCommittedHorizonRead();
				s_ReadCommittedHorizon = false;
			}
		}
		PathFinder* m_Finder = nullptr;
		bool m_Enable = false;
	};
}

unsigned char RTE::HorizonTerrainPatch::Sample(int x, int y) const {
	if (wrapsX && sceneWidth > 0) {
		x %= sceneWidth;
		if (x < 0) {
			x += sceneWidth;
		}
	}
	if (wrapsY && sceneHeight > 0) {
		y %= sceneHeight;
		if (y < 0) {
			y += sceneHeight;
		}
	}
	const int localX = x - originX;
	const int localY = y - originY;
	if (width <= 0 || height <= 0 || localX < 0 || localY < 0 || localX >= width || localY >= height) {
		return 0;
	}
	return pixels[static_cast<size_t>(localY) * static_cast<size_t>(width) + static_cast<size_t>(localX)];
}

RTE::PathNode::PathNode(const Vector& pos) :
    Pos(pos), m_Navigable(true) {
	const Material* outOfBounds = g_SceneMan.GetMaterialFromID(MaterialColorKeys::g_MaterialOutOfBounds);
	for (int i = 0; i < c_MaxAdjacentNodeCount; i++) {
		AdjacentNodes[i] = nullptr;
		AdjacentNodeBlockingMaterials[i] = outOfBounds; // Costs are infinite unless recalculated as otherwise.
	}
}

PathFinder::PathFinder(int nodeDimension) {
	Clear();
	Create(nodeDimension);
}

PathFinder::~PathFinder() {
	Destroy();
}

std::string PathFinder::SaveCheckpoint() const {
	WaitForPathingRequests();
	CheckpointWriter writer("PathFinder1");
	writer(m_NodeDimension, m_Offset, m_GridWidth, m_GridHeight, m_WrapsX, m_WrapsY, m_NodeGrid.size());
	for (const PathNode& node: m_NodeGrid) {
		writer(node.Pos, node.m_Navigable);
		for (const PathNode* adjacent: node.AdjacentNodes) writer(adjacent ? static_cast<int64_t>(adjacent - m_NodeGrid.data()) : int64_t{-1});
		for (const Material* material: node.AdjacentNodeBlockingMaterials) writer(material ? static_cast<int>(material->GetIndex()) : -1);
	}
	return writer.Text();
}

bool PathFinder::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "PathFinder1", validateOnly);
		unsigned dimension;
		Vector offset;
		int width, height;
		bool wrapsX, wrapsY;
		size_t count;
		reader.Value(dimension); reader.Value(offset); reader.Value(width); reader.Value(height);
		reader.Value(wrapsX); reader.Value(wrapsY); reader.Value(count);
		if (!dimension || width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height != count || count > text.size() / 8) return false;
		struct NodeState { Vector pos; bool navigable; std::array<int64_t, 8> adjacent; std::array<int, 8> material; };
		std::vector<NodeState> nodes(count);
		for (NodeState& node: nodes) {
			reader.Value(node.pos); reader.Value(node.navigable); reader.Value(node.adjacent); reader.Value(node.material);
			for (int64_t index: node.adjacent) if (index < -1 || (index >= 0 && static_cast<uint64_t>(index) >= count)) return false;
			for (int index: node.material) if (index < -1 || index >= 256) return false;
		}
		reader.Finish();
		if (validateOnly) return true;
		ClearHorizonState();
		// PathNode contains references to its own array elements: construct in place,
		// then swap storage, so neither vector growth nor a value copy can dangle them.
		std::vector<PathNode> replacement;
		replacement.reserve(count);
		for (const NodeState& node: nodes) replacement.emplace_back(node.pos);
		for (size_t index = 0; index < count; ++index) {
			PathNode& target = replacement[index];
			const NodeState& node = nodes[index];
			target.m_Navigable = node.navigable;
			for (size_t direction = 0; direction < 8; ++direction) {
				target.AdjacentNodes[direction] = node.adjacent[direction] < 0 ? nullptr : &replacement[node.adjacent[direction]];
				target.AdjacentNodeBlockingMaterials[direction] = node.material[direction] < 0 ? nullptr : g_SceneMan.GetMaterialFromID(static_cast<unsigned char>(node.material[direction]));
			}
		}
		m_NodeGrid.swap(replacement);
		m_NodeDimension = dimension; m_Offset = offset; m_GridWidth = width; m_GridHeight = height;
		m_WrapsX = wrapsX; m_WrapsY = wrapsY;
		// Every CalculatePathImpl already resets its thread's scratch pather before use.
		return true;
	} catch (const std::exception&) { return false; }
}

void PathFinder::ClearHorizonState() {
	WaitForPathingRequests();
	{
		std::lock_guard lock(m_HorizonMutex);
		for (const auto& job: m_HorizonJobs) {
			while (!job->ready.load()) {
				std::this_thread::yield();
			}
		}
		m_HorizonJobs.clear();
		m_HorizonNodes.clear();
	}
	m_HorizonGeneration.fetch_add(1);
	m_LastHorizonWaitUs = 0;
	m_LastHorizonExpired = 0;
	m_HorizonWorkerDelayMs = 0;
}

void PathFinder::Clear() {
	ClearHorizonState();
	m_NodeGrid.clear();
	m_Pather = nullptr;
	m_GridWidth = m_GridHeight = 0;
	m_WrapsX = m_WrapsY = false;
	m_NodeDimension = SCENEGRIDSIZE;
	m_Offset = Vector();
	m_SelfTestGrid = false;
}

int PathFinder::Create(int nodeDimension) {
	RTEAssert(g_SceneMan.GetScene(), "Scene doesn't exist or isn't loaded when creating PathFinder!");

	m_NodeDimension = nodeDimension;
	int sceneWidth = g_SceneMan.GetSceneWidth();
	int sceneHeight = g_SceneMan.GetSceneHeight();

	// Make overlapping nodes at seams if necessary, to make sure all scene pixels are covered.
	m_GridWidth = std::ceil(static_cast<float>(sceneWidth) / static_cast<float>(m_NodeDimension));
	m_GridHeight = std::ceil(static_cast<float>(sceneHeight) / static_cast<float>(m_NodeDimension));

	m_WrapsX = g_SceneMan.SceneWrapsX();
	m_WrapsY = g_SceneMan.SceneWrapsY();

	m_Offset = Vector(nodeDimension * 0.5f, 0.0F);

	// Create and assign scene coordinate positions for all nodes.
	Vector nodePos = Vector(static_cast<float>(nodeDimension) / 2.0F, static_cast<float>(nodeDimension) / 2.0F) + m_Offset;
	m_NodeGrid.reserve(m_GridWidth * m_GridHeight);
	for (int y = 0; y < m_GridHeight; ++y) {
		// Make sure no cell centers are off the scene (since they can overlap the far edge of the scene).
		if (nodePos.m_Y >= sceneHeight) {
			nodePos.m_Y = sceneHeight - 1.0F;
		}

		// Start the row over at middle of the leftmost node each new row.
		nodePos.m_X = static_cast<float>(nodeDimension) / 2.0F;

		for (int x = 0; x < m_GridWidth; ++x) {
			// Make sure no cell centers are off the scene (since they can overlap the far edge of the scene).
			if (nodePos.m_X >= sceneWidth) {
				nodePos.m_X = sceneWidth - 1.0F;
			}

			// Add the newly created node to the column.
			// Warning! Emplace back must be used to ensure this is constructed in-place, as otherwise the Up/Right/Down etc references will be incorrect.
			m_NodeGrid.emplace_back(nodePos);

			nodePos.m_X += static_cast<float>(nodeDimension);
		}

		nodePos.m_Y += static_cast<float>(nodeDimension);
	}

	// Assign all the adjacent nodes on each node. GetPathNodeAtGridCoords handles Scene wrapping.
	for (int x = 0; x < m_GridWidth; ++x) {
		for (int y = 0; y < m_GridHeight; ++y) {
			PathNode& node = *GetPathNodeAtGridCoords(x, y);

			node.Up = GetPathNodeAtGridCoords(x, y - 1);
			node.Right = GetPathNodeAtGridCoords(x + 1, y);
			node.Down = GetPathNodeAtGridCoords(x, y + 1);
			node.Left = GetPathNodeAtGridCoords(x - 1, y);
			node.UpRight = GetPathNodeAtGridCoords(x + 1, y - 1);
			node.RightDown = GetPathNodeAtGridCoords(x + 1, y + 1);
			node.DownLeft = GetPathNodeAtGridCoords(x - 1, y + 1);
			node.LeftUp = GetPathNodeAtGridCoords(x - 1, y - 1);
		}
	}

	RecalculateAllCosts();

	return 0;
}

void PathFinder::Destroy() {
	Clear();
}

MicroPather* PathFinder::GetPather() {
	// TODO: cache a collection of pathers. For async pathfinding right now we create a new pather for every thread!
	if (!s_Pather.m_Instance || s_Pather.m_Instance->GetGraph() != this) {
		// First time this thread has asked for a pather, let's initialize it
		delete s_Pather.m_Instance; // Might be reinitialized and Graph ptrs mismatch, in that case delete the old one

		// TODO: test dynamically setting this. The code below sets it based on map area and block size, with a hefty upper limit.
		// int sceneArea = m_GridWidth * m_GridHeight;
		// unsigned int numberOfBlocksToAllocate = std::min(128000, sceneArea / (m_NodeDimension * m_NodeDimension));
		unsigned int numberOfBlocksToAllocate = 4000;
		s_Pather.m_Instance = new MicroPather(this, numberOfBlocksToAllocate, PathNode::c_MaxAdjacentNodeCount, false);
	}

	return s_Pather.m_Instance;
}

int PathFinder::CalculatePath(Vector start, Vector end, std::list<Vector>& pathResult, float& totalCostResult, float jumpHeight, float digStrength, bool committedHorizon) {
	++m_CurrentPathingRequests;
	PathingRequestScope scope{m_CurrentPathingRequests};
	HorizonReadScope horizon(this, committedHorizon);
	return CalculatePathImpl(start, end, pathResult, totalCostResult, jumpHeight, digStrength);
}

int PathFinder::CalculatePathImpl(Vector start, Vector end, std::list<Vector>& pathResult, float& totalCostResult, float jumpHeight, float digStrength) {
	ZoneScoped;

	// Make sure start and end are within scene bounds.
	if (!m_SelfTestGrid) {
		g_SceneMan.ForceBounds(start);
		g_SceneMan.ForceBounds(end);
	}

	// Convert from absolute scene pixel coordinates to path node indices.
	int startNodeX = std::floor(start.m_X / static_cast<float>(m_NodeDimension));
	int startNodeY = std::max(0.0F, std::floor((start.m_Y / static_cast<float>(m_NodeDimension) - 0.5f)));
	int endNodeX = std::floor(end.m_X / static_cast<float>(m_NodeDimension));
	int endNodeY = std::max(0.0F, std::floor((end.m_Y / static_cast<float>(m_NodeDimension) - 0.5f)));

	// Clear out the results if it happens to contain anything
	pathResult.clear();

	// Due to different actors having different dig strengths, node costs aren't consistent, so reset on every path.
	GetPather()->Reset();

	// Actors capable of jumping/jetpacking can jump upwards.
	s_JumpHeight = jumpHeight;

	// How high up we can jump from this node.
	if(jumpHeight == FLT_MAX) {
		// Probably quite high.
		s_JumpHeightVertical = INT_MAX;
		s_JumpHeightDiagonal = INT_MAX;
	} else {
		// Assume at least 1 so automovers work a bit better
		s_JumpHeightVertical = std::max(1, static_cast<int>(jumpHeight / (m_NodeDimension * c_MPP)));
		s_JumpHeightDiagonal = std::max(1, static_cast<int>((jumpHeight * 0.7F) / (m_NodeDimension * c_MPP)));
	}

	// Actors capable of digging can use s_DigStrength to modify the node adjacency cost.
	s_DigStrength = digStrength;

	// Do the actual pathfinding, fetch out the list of states that comprise the best path.
	int result = MicroPather::NO_SOLUTION;
	std::vector<void*> statePath;

	// If end node is invalid, there's no path
	PathNode* endNode = GetPathNodeAtGridCoords(endNodeX, endNodeY);
	if (endNode && ViewNode(endNode).navigable) {
		result = GetPather()->Solve(static_cast<void*>(GetPathNodeAtGridCoords(startNodeX, startNodeY)), static_cast<void*>(endNode), &statePath, &totalCostResult);
	}

	if (result == MicroPather::NO_SOLUTION) {
		// Otherwise micropather inits it to zero :)
		totalCostResult = std::numeric_limits<float>::max();
	}

	if (!statePath.empty()) {
		// Replace the approximate first point from the pathfound path with the exact starting point.
		pathResult.push_back(start);
		std::vector<void*>::iterator itr = statePath.begin();
		itr++;

		// Convert from a list of state void pointers to a list of scene position vectors.
		for (; itr != statePath.end(); ++itr) {
			pathResult.push_back((static_cast<PathNode*>(*itr))->Pos);
		}

		// Adjust the last point to be exactly where the end is supposed to be (really?).
		pathResult.pop_back();
		pathResult.push_back(end);
	} else {
		// Empty path, give exact start and end.
		pathResult.push_back(start);
		pathResult.push_back(end);
	}

	// TODO: Clean up the path, remove series of nodes in the same direction etc?
	return result;
}

std::shared_ptr<volatile PathRequest> PathFinder::CalculatePathAsync(Vector start, Vector end, float jumpHeight, float digStrength, PathCompleteCallback callback, bool committedHorizon) {
	std::shared_ptr<volatile PathRequest> pathRequest = std::make_shared<PathRequest>();

	const_cast<Vector&>(pathRequest->startPos) = start;
	const_cast<Vector&>(pathRequest->targetPos) = end;

	++m_CurrentPathingRequests;
	try {
		g_ThreadMan.GetBackgroundThreadPool().push_task(
		    [this, start, end, jumpHeight, digStrength, callback, committedHorizon](std::shared_ptr<volatile PathRequest> volRequest) {
			    PathingRequestScope scope{m_CurrentPathingRequests};
			    HorizonReadScope horizon(this, committedHorizon);
			    // Cast away the volatile-ness - only matters outside (and complicates the API otherwise)
			    PathRequest& request = const_cast<PathRequest&>(*volRequest);

			    int status = CalculatePathImpl(start, end, request.path, request.totalCost, jumpHeight, digStrength);

			    request.status = status;
			    request.pathLength = request.path.size();

			    if (callback) {
				    callback(volRequest);
			    }

			    // Have to set to complete after the callback, so anything that blocks on it knows that the callback will have been called by now
			    // This has the awkward side-effect that the complete flag is actually false during the callback - but that's fine, if it's called we know it's complete anyways
			    request.complete = true;
		    },
		    pathRequest);
	} catch (...) {
		--m_CurrentPathingRequests;
		throw;
	}

	return pathRequest;
}

void PathFinder::WaitForPathingRequests() const {
	while (m_CurrentPathingRequests.load() != 0) std::this_thread::yield();
}

void PathFinder::RecalculateAllCosts() {
	RTEAssert(g_SceneMan.GetScene(), "Scene doesn't exist or isn't loaded when recalculating PathFinder!");

	// Deadlock until all path requests are complete
	while (m_CurrentPathingRequests.load() != 0) {};

	ClearHorizonState();

	// I hate this copy, but fuck it.
	std::vector<int> pathNodesIdsVec;
	pathNodesIdsVec.reserve(m_NodeGrid.size());
	for (size_t i = 0; i < m_NodeGrid.size(); ++i) {
		pathNodesIdsVec.push_back(i);
	}

	UpdateNodeList(pathNodesIdsVec);
}

std::vector<int> PathFinder::RecalculateAreaCosts(std::deque<Box>& boxList, size_t nodeUpdateLimit) {
	ZoneScoped;

	std::unordered_set<int> nodeIDsToUpdate;

	while (!boxList.empty()) {
		std::vector<int> nodesInside = GetNodeIdsInBox(boxList.front());
		for (int nodeId: nodesInside) {
			nodeIDsToUpdate.insert(nodeId);
		}

		boxList.pop_front();
		if (nodeIDsToUpdate.size() > nodeUpdateLimit) {
			break;
		}
	}

	// Note - This copy is necessary because std::for_each with parallel execution doesn't appear to work with std::unordered_set -
	// Using it will cause nodes to randomly fail to update. This should be rechecked when the codebase upgrades to C++20,
	// and then UpdateNodeList can be refactored to take a pair of iterators instead of a vector.
	std::vector<int> nodeVec(nodeIDsToUpdate.begin(), nodeIDsToUpdate.end());

	// If no PathNode costs were changed, clear the set of IDs to update, so it's empty when it's returned.
	if (!UpdateNodeList(nodeVec)) {
		nodeVec.clear();
	}

	return nodeVec;
}

float PathFinder::LeastCostEstimate(void* startState, void* endState) {
	const PathNode* startNode = static_cast<PathNode*>(startState);
	const PathNode* endNode = static_cast<PathNode*>(endState);
	return g_SceneMan.ShortestDistance(startNode->Pos, endNode->Pos).GetMagnitude() / m_NodeDimension;
}

void PathFinder::AdjacentCost(void* state, std::vector<micropather::StateCost>* adjacentList) {
	const PathNode* node = static_cast<PathNode*>(state);
	const NodeCostView view = ViewNode(node);
	micropather::StateCost adjCost;

	// We do a little trick here, where we radiate out a little percentage of our average cost in all directions.
	// This encourages the AI to generally try to give hard surfaces some berth when pathing, so we don't get too close and get stuck.
	const float costRadiationMultiplier = 0.2F;
	float radiatedCost = 0.0F; // GetNodeAverageTransitionCost(*node) * costRadiationMultiplier;

	bool isInNoGrav = !m_SelfTestGrid && g_SceneMan.IsPointInNoGravArea(node->Pos);
	bool allowDiagonal = !isInNoGrav; // We don't allow diagonals in nograv to improve automover behaviour

	auto neighborOpen = [this](PathNode* neighbor) {
		return neighbor && ViewNode(neighbor).navigable;
	};

	if (neighborOpen(node->Down)) {
		adjCost.cost = 1.0F + GetMaterialTransitionCost(*view.materials[4]) + radiatedCost;
		adjCost.state = static_cast<void*>(node->Down);
		adjacentList->push_back(adjCost);
	}

	if (neighborOpen(node->RightDown) && allowDiagonal) {
		adjCost.cost = 1.4F + (GetMaterialTransitionCost(*view.materials[3]) * 1.4F) + radiatedCost;
		adjCost.state = static_cast<void*>(node->RightDown);
		adjacentList->push_back(adjCost);
	}

	if (neighborOpen(node->DownLeft) && allowDiagonal) {
		adjCost.cost = 1.4F + (GetMaterialTransitionCost(*view.materials[5]) * 1.4F) + radiatedCost;
		adjCost.state = static_cast<void*>(node->DownLeft);
		adjacentList->push_back(adjCost);
	}

	if (isInNoGrav || NodeIsOnSolidGround(*node)) {
		// Cost to discourage us from going up
		const float extraUpCost = 3.0F;

		// We can only go straight left or right if we're on solid ground, otherwise we need to go downwards
		if (neighborOpen(node->Left)) {
			adjCost.cost = 1.0F + GetMaterialTransitionCost(*view.materials[6]) + radiatedCost;
			adjCost.state = static_cast<void*>(node->Left);
			adjacentList->push_back(adjCost);
		}

		if (neighborOpen(node->Right)) {
			adjCost.cost = 1.0F + GetMaterialTransitionCost(*view.materials[2]) + radiatedCost;
			adjCost.state = static_cast<void*>(node->Right);
			adjacentList->push_back(adjCost);
		}

		// Jumping vertically
		if (s_JumpHeight < FLT_MAX) {
			// How high up we can jump from this node
			const PathNode* currentNode = node;
			float totalMaterialCost = 0.0F;
			for (int i = 0; i < s_JumpHeightVertical; ++i) {
				const NodeCostView currentView = ViewNode(currentNode);
				if (currentNode->Up == nullptr || !ViewNode(currentNode->Up).navigable || currentView.materials[0]->GetIntegrity() > c_PathFindingDefaultDigStrength) {
					// solid ceiling, stop
					break;
				}

				float f = i + 2; // Exponential cost increase for jumping higher
				float extraJumpCost = f * f * 0.5F; // Exponential cost increase for jumping higher

				totalMaterialCost += 1.0F + extraUpCost + extraJumpCost + (GetMaterialTransitionCost(*currentView.materials[0]) * 3.0F) + radiatedCost;

				adjCost.cost = totalMaterialCost;
				adjCost.state = static_cast<void*>(currentNode->Up);
				adjacentList->push_back(adjCost);

				currentNode = currentNode->Up;
			}
		} else if (neighborOpen(node->Up)) {
			adjCost.cost = 1.0F + (extraUpCost) + (GetMaterialTransitionCost(*view.materials[1]) * 3.0F) + radiatedCost; // Three times more expensive when digging.
			adjCost.state = static_cast<void*>(node->Up);
			adjacentList->push_back(adjCost);
		}

		// Jumping diagonally
		if (s_JumpHeight < FLT_MAX && node->UpRight && !isInNoGrav) {
			const PathNode* currentNode = node->UpRight;
			float totalMaterialCost = 1.4F + (extraUpCost * 1.4F) + (GetMaterialTransitionCost(*view.materials[1]) * 1.4F * 3.0F) + radiatedCost;
			for (int i = 0; i < s_JumpHeightDiagonal; ++i) {
				const NodeCostView currentView = ViewNode(currentNode);
				if (currentNode->UpRight == nullptr || !ViewNode(currentNode->UpRight).navigable || currentView.materials[1]->GetIntegrity() > c_PathFindingDefaultDigStrength) {
					// solid ceiling, stop
					break;
				}

				float f = i + 2; // Exponential cost increase for jumping higher
				float extraJumpCost = f * f * 0.5F; // Exponential cost increase for jumping higher

				totalMaterialCost += 1.4F + (extraUpCost * 1.4F) + (extraJumpCost * 1.4f) + (GetMaterialTransitionCost(*currentView.materials[1]) * 1.4F * 3.0F) + radiatedCost;

				adjCost.cost = totalMaterialCost;
				adjCost.state = static_cast<void*>(currentNode->UpRight);
				adjacentList->push_back(adjCost);

				currentNode = currentNode->UpRight;
			}
		}

		if (s_JumpHeight < FLT_MAX && node->LeftUp && !isInNoGrav) {
			const PathNode* currentNode = node->LeftUp;
			float totalMaterialCost = 1.4F + (extraUpCost * 1.4F) + (GetMaterialTransitionCost(*view.materials[7]) * 1.4F * 3.0F) + radiatedCost;
			for (int i = 0; i < s_JumpHeightDiagonal; ++i) {
				const NodeCostView currentView = ViewNode(currentNode);
				if (currentNode->LeftUp == nullptr || !ViewNode(currentNode->LeftUp).navigable || currentView.materials[7]->GetIntegrity() > c_PathFindingDefaultDigStrength) {
					// solid ceiling, stop
					break;
				}

				float f = i + 2; // Exponential cost increase for jumping higher
				float extraJumpCost = f * f * 0.5F; // Exponential cost increase for jumping higher

				totalMaterialCost += 1.4F + (extraUpCost * 1.4F) + (extraJumpCost * 1.4f) + (GetMaterialTransitionCost(*currentView.materials[7]) * 1.4F * 3.0F) + radiatedCost;

				adjCost.cost = totalMaterialCost;
				adjCost.state = static_cast<void*>(currentNode->LeftUp);
				adjacentList->push_back(adjCost);

				currentNode = currentNode->LeftUp;
			}
		}

		// Add cost for digging at 45 degrees and for digging upwards.
		if (neighborOpen(node->UpRight) && allowDiagonal) {
			adjCost.cost = 1.4F + (extraUpCost * 1.4F) + (GetMaterialTransitionCost(*view.materials[1]) * 1.4F * 3.0F) + radiatedCost; // Three times more expensive when digging.
			adjCost.state = static_cast<void*>(node->UpRight);
			adjacentList->push_back(adjCost);
		}

		if (neighborOpen(node->LeftUp) && allowDiagonal) {
			adjCost.cost = 1.4F + (extraUpCost * 1.4F) + (GetMaterialTransitionCost(*view.materials[7]) * 1.4F * 3.0F) + radiatedCost; // Three times more expensive when digging.
			adjCost.state = static_cast<void*>(node->LeftUp);
			adjacentList->push_back(adjCost);
		}
	}
}

bool PathFinder::PositionsAreTheSamePathNode(const Vector& pos1, const Vector& pos2) const {
	int startNodeX = std::floor(pos1.m_X / static_cast<float>(m_NodeDimension));
	int startNodeY = std::floor(pos1.m_Y / static_cast<float>(m_NodeDimension));
	int endNodeX = std::floor(pos2.m_X / static_cast<float>(m_NodeDimension));
	int endNodeY = std::floor(pos2.m_Y / static_cast<float>(m_NodeDimension));
	return startNodeX == endNodeX && startNodeY == endNodeY;
}

bool PathFinder::NodeIsOnSolidGround(const PathNode& node) const {
	const NodeCostView view = ViewNode(&node);
	return s_JumpHeight == FLT_MAX || (node.Down && view.materials[4]->GetIntegrity() > c_PathFindingDefaultDigStrength);
}

float PathFinder::GetMaterialTransitionCost(const Material& material) const {
	float strength = material.GetIntegrity();

	// Always treat doors as diggable.
	if (strength > s_DigStrength && material.GetIndex() != MaterialColorKeys::g_MaterialDoor) {
		strength *= 1000.0F;
	}

	return strength;
}

const Material* PathFinder::StrongestMaterialAlongLine(const Vector& start, const Vector& end) const {
	return g_SceneMan.CastMaxStrengthRayMaterial(start, end, 0, MaterialColorKeys::g_MaterialAir);
}

bool PathFinder::UpdateNodeCosts(PathNode* node) const {
	if (!node) {
		return false;
	}

	std::array<const Material*, PathNode::c_MaxAdjacentNodeCount> oldMaterials = node->AdjacentNodeBlockingMaterials;

	auto getStrongerMaterial = [](const Material* first, const Material* second) {
		return first->GetIntegrity() > second->GetIntegrity() ? first : second;
	};

	// Look at each existing adjacent node and calculate the cost for each. Start and end are offset to cover more terrain.
	// Note that we only calculate transitions to one side (down and right), because for the other side we can pull our up-and-left transition data from the other node's down-and-right.
	if (node->Right) {
		Vector offset(0.0F, 3.0F);
		node->RightMaterial = getStrongerMaterial(StrongestMaterialAlongLine(node->Pos - offset, node->Right->Pos - offset), StrongestMaterialAlongLine(node->Pos + offset, node->Right->Pos + offset));
	}

	if (node->Down) {
		Vector offset(3.0F, 0.0F);
		node->DownMaterial = getStrongerMaterial(StrongestMaterialAlongLine(node->Pos - offset, node->Down->Pos - offset), StrongestMaterialAlongLine(node->Pos + offset, node->Down->Pos + offset));
	}

	if (node->UpRight) {
		Vector offset(2.0F, 2.0F);
		node->UpRightMaterial = getStrongerMaterial(StrongestMaterialAlongLine(node->Pos - offset, node->UpRight->Pos - offset), StrongestMaterialAlongLine(node->Pos + offset, node->UpRight->Pos + offset));
	}

	if (node->RightDown) {
		Vector offset(2.0F, -2.0F);
		node->RightDownMaterial = getStrongerMaterial(StrongestMaterialAlongLine(node->Pos - offset, node->RightDown->Pos - offset), StrongestMaterialAlongLine(node->Pos + offset, node->RightDown->Pos + offset));
	}

	for (int i = 0; i < PathNode::c_MaxAdjacentNodeCount; ++i) {
		const Material* oldMat = oldMaterials[i];
		const Material* newMat = node->AdjacentNodeBlockingMaterials[i];

		// Check if the material strength is more than our delta, or if a door has appeared/disappeared (since we handle their costs in a special manner).
		float delta = std::abs(oldMat->GetIntegrity() - newMat->GetIntegrity());
		bool doorChanged = oldMat != newMat && (oldMat->GetIndex() == MaterialColorKeys::g_MaterialDoor || newMat->GetIndex() == MaterialColorKeys::g_MaterialDoor);
		if (delta > c_NodeCostChangeEpsilon || doorChanged) {
			return true;
		}
	}

	// None of the updates was past our epsilon, so ignore it and pretend it never happened.
	node->AdjacentNodeBlockingMaterials = oldMaterials;
	return false;
}

std::vector<int> PathFinder::GetNodeIdsInBox(Box box) {
	std::vector<int> result;

	box.Unflip();

	// Get the extents of the box's potential influence on PathNodes and their connecting edges.
	int firstX = static_cast<int>(std::floor((box.m_Corner.m_X / static_cast<float>(m_NodeDimension)) + 0.5F) - 1);
	int lastX = static_cast<int>(std::floor(((box.m_Corner.m_X + box.m_Width) / static_cast<float>(m_NodeDimension)) + 0.5F) + 1);
	int firstY = static_cast<int>(std::floor((box.m_Corner.m_Y / static_cast<float>(m_NodeDimension)) + 0.5F) - 1);
	int lastY = static_cast<int>(std::floor(((box.m_Corner.m_Y + box.m_Height) / static_cast<float>(m_NodeDimension)) + 0.5F) + 1);

	// Only iterate through the grid where the box overlaps any edges.
	for (int nodeX = firstX; nodeX <= lastX; ++nodeX) {
		for (int nodeY = firstY; nodeY <= lastY; ++nodeY) {
			int nodeId = ConvertCoordsToNodeId(nodeX, nodeY);
			if (nodeId != -1) {
				result.push_back(nodeId);
			}
		}
	}

	return result;
}

float PathFinder::GetNodeAverageTransitionCost(const PathNode& node) const {
	float totalCostOfAdjacentNodes = 0.0F;
	int count = 0;
	for (const Material* material: node.AdjacentNodeBlockingMaterials) {
		// Don't use node transition cost, because we don't care about digging.
		float cost = material->GetIntegrity();
		if (cost < std::numeric_limits<float>::max()) {
			totalCostOfAdjacentNodes += cost;
			count++;
		}
	}

	return totalCostOfAdjacentNodes / std::max(static_cast<float>(count), 1.0F);
}

bool PathFinder::UpdateNodeList(const std::vector<int>& nodeVec) {
	ZoneScoped;

	std::atomic<bool> anyChange = false;

	// Update all the costs going out from each node.
	std::for_each(
	    std::execution::par_unseq,
	    nodeVec.begin(),
	    nodeVec.end(),
	    [this, &anyChange](int nodeId) {
		    if (UpdateNodeCosts(&m_NodeGrid[nodeId])) {
			    anyChange = true;
		    }
	    });

	if (anyChange) {
		// UpdateNodeCosts only calculates Materials for Right and Down directions, so each PathNode's Up and Left direction Materials need to be matched to the respective neighbor's opposite direction Materials.
		// For example, this PathNode's Left Material is its Left neighbor's Right Material.
		std::for_each(
		    std::execution::par_unseq,
		    nodeVec.begin(),
		    nodeVec.end(),
		    [this](int nodeId) {
			    PathNode* node = &m_NodeGrid[nodeId];
			    if (node->Right) {
				    node->Right->LeftMaterial = node->RightMaterial;
			    }
			    if (node->Down) {
				    node->Down->UpMaterial = node->DownMaterial;
			    }
			    if (node->UpRight) {
				    node->UpRight->DownLeftMaterial = node->UpRightMaterial;
			    }
			    if (node->RightDown) {
				    node->RightDown->LeftUpMaterial = node->RightDownMaterial;
			    }
		    });
	}

	return anyChange;
}

void PathFinder::MarkBoxNavigable(Box box, bool navigable) {
	std::vector<int> pathNodesInBox = GetNodeIdsInBox(box);
	std::for_each(
	    std::execution::par_unseq,
	    pathNodesInBox.begin(),
	    pathNodesInBox.end(),
	    [this, navigable](int nodeId) {
		    PathNode* node = &m_NodeGrid[nodeId];
		    node->m_Navigable = navigable;
	    });
}

void PathFinder::MarkAllNodesNavigable(bool navigable) {
	std::vector<int> pathNodesIdsVec;
	pathNodesIdsVec.reserve(m_NodeGrid.size());
	for (size_t i = 0; i < m_NodeGrid.size(); ++i) {
		pathNodesIdsVec.push_back(i);
	}

	std::for_each(
	    std::execution::par_unseq,
	    pathNodesIdsVec.begin(),
	    pathNodesIdsVec.end(),
	    [this, navigable](int nodeId) {
		    PathNode* node = &m_NodeGrid[nodeId];
		    node->m_Navigable = navigable;
	    });
}

RTE::PathNode* PathFinder::GetPathNodeAtGridCoords(int x, int y) {
	int nodeId = ConvertCoordsToNodeId(x, y);
	return nodeId != -1 ? &m_NodeGrid[nodeId] : nullptr;
}

int PathFinder::ConvertCoordsToNodeId(int x, int y) const {
	if (m_WrapsX) {
		x = x % m_GridWidth;
		x = x < 0 ? x + m_GridWidth : x;
	}

	if (m_WrapsY) {
		y = y % m_GridHeight;
		y = y < 0 ? y + m_GridHeight : y;
	}

	if (x < 0 || x >= m_GridWidth || y < 0 || y >= m_GridHeight) {
		return -1;
	}

	return (y * m_GridWidth) + x;
}

PathFinder::NodeCostView PathFinder::ViewNode(const PathNode* node) const {
	NodeCostView view;
	if (!node) {
		return view;
	}
	view.materials = node->AdjacentNodeBlockingMaterials;
	view.navigable = node->m_Navigable;
	if (!s_ReadCommittedHorizon || m_NodeGrid.empty()) {
		return view;
	}
	const int nodeId = static_cast<int>(node - m_NodeGrid.data());
	if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
		return view;
	}
	std::lock_guard lock(m_HorizonMutex);
	const auto found = m_HorizonNodes.find(nodeId);
	if (found != m_HorizonNodes.end()) {
		view.materials = found->second.committedMaterials;
		view.navigable = found->second.committedNavigable;
	}
	return view;
}

const Material* PathFinder::StrongestMaterialAlongPatch(const Vector& start, const Vector& end, const std::vector<HorizonTerrainPatch>& patches) const {
	const Material* strongest = g_SceneMan.GetMaterialFromID(MaterialColorKeys::g_MaterialAir);
	if (!strongest) {
		return strongest;
	}
	auto sample = [&](int x, int y) -> unsigned char {
		for (const HorizonTerrainPatch& patch: patches) {
			int sx = x;
			int sy = y;
			if (patch.wrapsX && patch.sceneWidth > 0) {
				sx %= patch.sceneWidth;
				if (sx < 0) {
					sx += patch.sceneWidth;
				}
			}
			if (patch.wrapsY && patch.sceneHeight > 0) {
				sy %= patch.sceneHeight;
				if (sy < 0) {
					sy += patch.sceneHeight;
				}
			}
			const int localX = sx - patch.originX;
			const int localY = sy - patch.originY;
			if (patch.width > 0 && patch.height > 0 && localX >= 0 && localY >= 0 && localX < patch.width && localY < patch.height) {
				return patch.pixels[static_cast<size_t>(localY) * static_cast<size_t>(patch.width) + static_cast<size_t>(localX)];
			}
		}
		return MaterialColorKeys::g_MaterialAir;
	};

	int intPos[2] = {static_cast<int>(std::floor(start.m_X)), static_cast<int>(std::floor(start.m_Y))};
	int delta[2] = {static_cast<int>(std::floor(end.m_X)) - intPos[0], static_cast<int>(std::floor(end.m_Y)) - intPos[1]};
	int increment[2] = {delta[0] < 0 ? -1 : 1, delta[1] < 0 ? -1 : 1};
	delta[0] = std::abs(delta[0]);
	delta[1] = std::abs(delta[1]);
	int delta2[2] = {delta[0] << 1, delta[1] << 1};
	const int dom = delta[0] > delta[1] ? 0 : 1;
	const int sub = 1 - dom;
	int error = delta2[sub] - delta[dom];
	for (int step = 0; step < delta[dom]; ++step) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];
		const unsigned char materialID = sample(intPos[0], intPos[1]);
		if (materialID != MaterialColorKeys::g_MaterialAir) {
			const Material* found = g_SceneMan.GetMaterialFromID(materialID);
			if (found && found->GetIntegrity() > strongest->GetIntegrity()) {
				strongest = found;
			}
		}
	}
	return strongest;
}

void PathFinder::ComputeHorizonMaterialsFromPatch(const std::vector<int>& nodeIds, const std::vector<HorizonTerrainPatch>& patches, std::vector<std::array<const Material*, 8>>& materials, std::vector<char>& navigable) const {
	materials.assign(nodeIds.size(), {});
	navigable.assign(nodeIds.size(), 1);
	std::unordered_map<int, std::array<const Material*, 8>> computed;
	computed.reserve(nodeIds.size());
	auto getStrongerMaterial = [](const Material* first, const Material* second) {
		return first->GetIntegrity() > second->GetIntegrity() ? first : second;
	};
	for (int nodeId: nodeIds) {
		if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
			continue;
		}
		const PathNode& node = m_NodeGrid[nodeId];
		std::array<const Material*, 8> next = node.AdjacentNodeBlockingMaterials;
		if (node.Right) {
			Vector offset(0.0F, 3.0F);
			next[2] = getStrongerMaterial(StrongestMaterialAlongPatch(node.Pos - offset, node.Right->Pos - offset, patches), StrongestMaterialAlongPatch(node.Pos + offset, node.Right->Pos + offset, patches));
		}
		if (node.Down) {
			Vector offset(3.0F, 0.0F);
			next[4] = getStrongerMaterial(StrongestMaterialAlongPatch(node.Pos - offset, node.Down->Pos - offset, patches), StrongestMaterialAlongPatch(node.Pos + offset, node.Down->Pos + offset, patches));
		}
		if (node.UpRight) {
			Vector offset(2.0F, 2.0F);
			next[1] = getStrongerMaterial(StrongestMaterialAlongPatch(node.Pos - offset, node.UpRight->Pos - offset, patches), StrongestMaterialAlongPatch(node.Pos + offset, node.UpRight->Pos + offset, patches));
		}
		if (node.RightDown) {
			Vector offset(2.0F, -2.0F);
			next[3] = getStrongerMaterial(StrongestMaterialAlongPatch(node.Pos - offset, node.RightDown->Pos - offset, patches), StrongestMaterialAlongPatch(node.Pos + offset, node.RightDown->Pos + offset, patches));
		}
		computed[nodeId] = next;
	}
	for (int nodeId: nodeIds) {
		auto found = computed.find(nodeId);
		if (found == computed.end()) {
			continue;
		}
		const PathNode& node = m_NodeGrid[nodeId];
		auto take = [&](PathNode* neighbor, int towardNeighbor, int towardHere) {
			if (!neighbor) {
				return;
			}
			const int neighborId = static_cast<int>(neighbor - m_NodeGrid.data());
			auto neighborFound = computed.find(neighborId);
			if (neighborFound != computed.end()) {
				neighborFound->second[towardHere] = found->second[towardNeighbor];
			}
		};
		take(node.Right, 2, 6);
		take(node.Down, 4, 0);
		take(node.UpRight, 1, 5);
		take(node.RightDown, 3, 7);
	}
	for (size_t index = 0; index < nodeIds.size(); ++index) {
		const int nodeId = nodeIds[index];
		if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
			continue;
		}
		const auto found = computed.find(nodeId);
		materials[index] = found != computed.end() ? found->second : m_NodeGrid[nodeId].AdjacentNodeBlockingMaterials;
		navigable[index] = m_NodeGrid[nodeId].m_Navigable ? 1 : 0;
	}
}

void PathFinder::LaunchHorizonWorker(const std::shared_ptr<HorizonJob>& job) {
	const int delayMs = m_HorizonWorkerDelayMs;
	const bool late = FaultInjected("horizon_worker_late");
	const bool compute = job->materials.empty();
	auto finish = [this, job, delayMs, late, compute]() {
		if (compute) {
			ComputeHorizonMaterialsFromPatch(job->nodeIds, job->patches, job->materials, job->navigable);
		}
		if (delayMs > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		}
		if (late) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		job->ready.store(true);
	};
	if (!compute && delayMs <= 0 && !late) {
		finish();
		return;
	}
	if (ThreadMan::IsConstructed()) {
		g_ThreadMan.GetBackgroundThreadPool().push_task(finish);
		return;
	}
	std::thread(finish).detach();
}

void PathFinder::ApplyHorizonJob(const HorizonJob& job) {
	for (size_t index = 0; index < job.nodeIds.size(); ++index) {
		const int nodeId = job.nodeIds[index];
		if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
			continue;
		}
		const PathNode& live = m_NodeGrid[nodeId];
		const bool liveMatches = live.AdjacentNodeBlockingMaterials == job.materials[index] && (live.m_Navigable ? 1 : 0) == job.navigable[index];
		if (liveMatches) {
			m_HorizonNodes.erase(nodeId);
			continue;
		}
		HorizonNode& overlay = m_HorizonNodes[nodeId];
		overlay.committedMaterials = job.materials[index];
		overlay.committedNavigable = job.navigable[index] != 0;
		overlay.generation = m_HorizonGeneration.load();
	}
}

void PathFinder::PinHorizonFromLive(const Box& box) {
	if (m_NodeGrid.empty()) {
		return;
	}
	std::lock_guard lock(m_HorizonMutex);
	for (int nodeId: GetNodeIdsInBox(box)) {
		if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size() || m_HorizonNodes.count(nodeId) != 0) {
			continue;
		}
		HorizonNode& overlay = m_HorizonNodes[nodeId];
		overlay.committedMaterials = m_NodeGrid[nodeId].AdjacentNodeBlockingMaterials;
		overlay.committedNavigable = m_NodeGrid[nodeId].m_Navigable;
		overlay.generation = m_HorizonGeneration.load();
	}
}

void PathFinder::CaptureHorizonPatch(const Box& box, HorizonTerrainPatch& patch) const {
	patch = {};
	Scene* scene = g_SceneMan.GetScene();
	if (!scene || !scene->GetTerrain() || !scene->GetTerrain()->GetMaterialBitmap()) {
		return;
	}
	Box area = box;
	area.Unflip();
	constexpr int pad = 4;
	const int x0 = static_cast<int>(std::floor(area.m_Corner.m_X)) - pad;
	const int y0 = static_cast<int>(std::floor(area.m_Corner.m_Y)) - pad;
	const int x1 = static_cast<int>(std::ceil(area.m_Corner.m_X + area.m_Width)) + pad;
	const int y1 = static_cast<int>(std::ceil(area.m_Corner.m_Y + area.m_Height)) + pad;
	patch.originX = x0;
	patch.originY = y0;
	patch.width = std::max(0, x1 - x0);
	patch.height = std::max(0, y1 - y0);
	patch.sceneWidth = g_SceneMan.GetSceneWidth();
	patch.sceneHeight = g_SceneMan.GetSceneHeight();
	patch.wrapsX = g_SceneMan.SceneWrapsX();
	patch.wrapsY = g_SceneMan.SceneWrapsY();
	if (patch.width <= 0 || patch.height <= 0) {
		return;
	}
	patch.pixels.resize(static_cast<size_t>(patch.width) * static_cast<size_t>(patch.height), MaterialColorKeys::g_MaterialAir);
	for (int y = 0; y < patch.height; ++y) {
		for (int x = 0; x < patch.width; ++x) {
			patch.pixels[static_cast<size_t>(y) * static_cast<size_t>(patch.width) + static_cast<size_t>(x)] = g_SceneMan.GetTerrMatter(patch.originX + x, patch.originY + y);
		}
	}
}

void PathFinder::QueueHorizonUpdate(uint64_t originTick, uint16_t horizonTicks, const std::vector<Box>& boxes, const std::vector<HorizonTerrainPatch>& patches) {
	if (boxes.empty() || horizonTicks == 0 || m_NodeGrid.empty()) {
		return;
	}
	std::unordered_set<int> nodeIds;
	for (const Box& box: boxes) {
		for (int nodeId: GetNodeIdsInBox(box)) {
			nodeIds.insert(nodeId);
		}
	}
	if (nodeIds.empty()) {
		return;
	}
	auto job = std::make_shared<HorizonJob>();
	job->originTick = originTick;
	job->commitTick = originTick + horizonTicks;
	job->nodeIds.assign(nodeIds.begin(), nodeIds.end());
	std::sort(job->nodeIds.begin(), job->nodeIds.end());
	job->patches = patches;
	job->ready.store(false);
	{
		std::lock_guard lock(m_HorizonMutex);
		m_HorizonJobs.push_back(job);
	}
	LaunchHorizonWorker(job);
}

void PathFinder::QueueHorizonDelta(uint64_t originTick, uint16_t horizonTicks, int nodeId, const std::array<const Material*, 8>& materials, bool navigable) {
	if (horizonTicks == 0 || nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
		return;
	}
	auto job = std::make_shared<HorizonJob>();
	job->originTick = originTick;
	job->commitTick = originTick + horizonTicks;
	job->nodeIds = {nodeId};
	job->materials = {materials};
	job->navigable = {static_cast<char>(navigable ? 1 : 0)};
	job->ready.store(false);
	{
		std::lock_guard lock(m_HorizonMutex);
		if (m_HorizonNodes.count(nodeId) == 0) {
			HorizonNode& overlay = m_HorizonNodes[nodeId];
			overlay.committedMaterials = m_NodeGrid[nodeId].AdjacentNodeBlockingMaterials;
			overlay.committedNavigable = m_NodeGrid[nodeId].m_Navigable;
		}
		m_HorizonJobs.push_back(job);
	}
	LaunchHorizonWorker(job);
}

void PathFinder::DrainCommittedHorizonReaders() {
	const auto started = std::chrono::steady_clock::now();
	while (m_CommittedHorizonReaders.load() != 0) {
		const int64_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		if (elapsed >= c_HorizonWaitCapUs) {
			break;
		}
		std::this_thread::yield();
	}
}

void PathFinder::CommitHorizonThrough(uint64_t nowTick) {
	DrainCommittedHorizonReaders();
	std::vector<std::shared_ptr<HorizonJob>> due;
	{
		std::lock_guard lock(m_HorizonMutex);
		for (const auto& job: m_HorizonJobs) {
			if (job->commitTick <= nowTick) {
				due.push_back(job);
			}
		}
	}
	std::sort(due.begin(), due.end(), [](const auto& lhs, const auto& rhs) {
		if (lhs->originTick != rhs->originTick) {
			return lhs->originTick < rhs->originTick;
		}
		return lhs->commitTick < rhs->commitTick;
	});
	for (const auto& job: due) {
		int64_t waitUs = 0;
		if (!job->ready.load()) {
			const auto started = std::chrono::steady_clock::now();
			while (!job->ready.load()) {
				waitUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
				if (waitUs >= c_HorizonWaitCapUs) {
					break;
				}
				std::this_thread::yield();
			}
			if (!job->ready.load()) {
				m_LastHorizonWaitUs = waitUs;
				m_LastHorizonExpired += 1;
				RecordHorizonWait(waitUs);
				std::cout << "[horizon-path] wait_us=" << waitUs << " tick=" << nowTick << " origin=" << job->originTick
				          << " expired=1 count=" << HorizonWaitCount() << " p99_us=" << HorizonWaitP99Us() << std::endl;
				std::lock_guard lock(m_HorizonMutex);
				m_HorizonJobs.erase(std::remove(m_HorizonJobs.begin(), m_HorizonJobs.end(), job), m_HorizonJobs.end());
				continue;
			}
		}
		m_LastHorizonWaitUs = waitUs;
		RecordHorizonWait(waitUs);
		std::cout << "[horizon-path] wait_us=" << waitUs << " tick=" << nowTick << " origin=" << job->originTick
		          << " count=" << HorizonWaitCount() << " p99_us=" << HorizonWaitP99Us() << std::endl;
		{
			std::lock_guard lock(m_HorizonMutex);
			ApplyHorizonJob(*job);
			m_HorizonGeneration.fetch_add(1);
			m_HorizonJobs.erase(std::remove(m_HorizonJobs.begin(), m_HorizonJobs.end(), job), m_HorizonJobs.end());
		}
	}
	if (!due.empty()) {
		WriteHorizonWaitReport();
	}
}

int64_t PathFinder::HorizonWaitCount() {
	auto& stats = HorizonStats();
	std::lock_guard lock(stats.mutex);
	return static_cast<int64_t>(stats.count);
}

int64_t PathFinder::HorizonWaitP99Us() {
	auto& stats = HorizonStats();
	std::lock_guard lock(stats.mutex);
	return HorizonP99Locked(stats);
}

void PathFinder::ResetHorizonWaitStats() {
	auto& stats = HorizonStats();
	std::lock_guard lock(stats.mutex);
	stats.count = 0;
	stats.lastUs = 0;
	stats.ringCount = 0;
	stats.ringNext = 0;
	stats.ring.fill(0);
}

void PathFinder::WriteHorizonWaitReport() {
	auto& stats = HorizonStats();
	int64_t count = 0;
	int64_t p99 = 0;
	int64_t last = 0;
	{
		std::lock_guard lock(stats.mutex);
		count = static_cast<int64_t>(stats.count);
		p99 = HorizonP99Locked(stats);
		last = stats.lastUs;
	}
	const std::string path = System::GetWorkingDirectory() + "Userdata/horizon_path_grid.json";
	std::ofstream out(path, std::ios::trunc);
	if (!out) {
		return;
	}
	out << "{\"count\":" << count << ",\"p99_us\":" << p99 << ",\"last_wait_us\":" << last << "}\n";
}

void PathFinder::TestInstallGrid(int width, int height, int nodeDimension, const Material* fill) {
	WaitForPathingRequests();
	{
		std::lock_guard lock(m_HorizonMutex);
		m_HorizonJobs.clear();
		m_HorizonNodes.clear();
	}
	m_SelfTestGrid = true;
	m_NodeDimension = static_cast<unsigned int>(nodeDimension);
	m_GridWidth = width;
	m_GridHeight = height;
	m_WrapsX = m_WrapsY = false;
	m_Offset = Vector(static_cast<float>(nodeDimension) * 0.5F, 0.0F);
	m_NodeGrid.clear();
	m_NodeGrid.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			m_NodeGrid.emplace_back(Vector(static_cast<float>(x * nodeDimension + nodeDimension / 2), static_cast<float>(y * nodeDimension + nodeDimension / 2)));
		}
	}
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			PathNode& node = *GetPathNodeAtGridCoords(x, y);
			node.Up = GetPathNodeAtGridCoords(x, y - 1);
			node.Right = GetPathNodeAtGridCoords(x + 1, y);
			node.Down = GetPathNodeAtGridCoords(x, y + 1);
			node.Left = GetPathNodeAtGridCoords(x - 1, y);
			node.UpRight = GetPathNodeAtGridCoords(x + 1, y - 1);
			node.RightDown = GetPathNodeAtGridCoords(x + 1, y + 1);
			node.DownLeft = GetPathNodeAtGridCoords(x - 1, y + 1);
			node.LeftUp = GetPathNodeAtGridCoords(x - 1, y - 1);
			for (int dir = 0; dir < PathNode::c_MaxAdjacentNodeCount; ++dir) {
				node.AdjacentNodeBlockingMaterials[dir] = fill;
			}
			node.m_Navigable = true;
		}
	}
}

void PathFinder::TestSetNodeMaterials(int nodeId, const std::array<const Material*, 8>& materials) {
	if (nodeId < 0 || static_cast<size_t>(nodeId) >= m_NodeGrid.size()) {
		return;
	}
	m_NodeGrid[nodeId].AdjacentNodeBlockingMaterials = materials;
}

void PathFinder::TestApplyLiveUpdate(int nodeId, const std::array<const Material*, 8>& materials) {
	if (m_CurrentPathingRequests.load() != 0) {
		return;
	}
	TestSetNodeMaterials(nodeId, materials);
}

int PathFinder::RunHorizonGridSelfTest() {
	constexpr const char* Tag = "[horizon-path-grid-selftest]";
	struct TestMaterial : Material {
		void Arm(unsigned char index, float integrity) {
			m_Index = index;
			m_Integrity = integrity;
		}
	};
	TestMaterial air;
	TestMaterial rock;
	air.Arm(0, 0.0F);
	rock.Arm(5, 200.0F);
	std::array<const Material*, 8> blocked{};
	blocked.fill(&rock);

	auto pathCost = [](PathFinder& finder, bool committed) {
		std::list<Vector> path;
		float cost = -1.0F;
		finder.CalculatePath(Vector(10, 50), Vector(150, 50), path, cost, FLT_MAX, 1.0F, committed);
		return std::pair<int, float>{static_cast<int>(path.size()), cost};
	};
	auto sceneCost = [](Scene& scene) {
		std::list<Vector> path;
		return scene.CalculatePath(Vector(10, 50), Vector(150, 50), path, FLT_MAX, 1.0F, Activity::Teams::NoTeam);
	};
	auto fail = [&](const char* text) {
		std::cout << Tag << " FAIL " << text << std::endl;
		g_CurrentAIActor = nullptr;
		TestArmFaultInject("");
		g_SceneMan.TestBindCurrentScene(nullptr);
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		return 1;
	};

	ResetHorizonWaitStats();
	PathFinder peerA;
	PathFinder peerB;
	peerA.TestInstallGrid(8, 4, 20, &air);
	peerB.TestInstallGrid(8, 4, 20, &air);
	const int wall = peerA.TestNodeIdAt(3, 2);
	if (wall < 0) {
		return fail("grid setup did not produce a wall node");
	}

	// (i) two peers, FaultInjected late worker, commit at T+H. Live split is the in-flight skip.
	TestArmFaultInject("horizon_worker_late");
	peerB.TestHoldPathingRequest();
	peerA.QueueHorizonDelta(10, 4, wall, blocked);
	peerB.QueueHorizonDelta(10, 4, wall, blocked);
	peerA.TestApplyLiveUpdate(wall, blocked);
	peerB.TestApplyLiveUpdate(wall, blocked);
	peerA.CommitHorizonThrough(14);
	peerB.CommitHorizonThrough(14);
	const auto sharedA = pathCost(peerA, true);
	const auto sharedB = pathCost(peerB, true);
	const auto liveA = pathCost(peerA, false);
	const auto liveB = pathCost(peerB, false);
	peerB.TestReleasePathingRequest();
	TestArmFaultInject("");
	if (sharedA != sharedB) {
		std::cout << Tag << " FAIL shared CalculatePath returned the per-machine answer a=" << sharedA.second << " b=" << sharedB.second << std::endl;
		return 1;
	}
	if (liveA == liveB) {
		return fail("two-peer live grids did not diverge under per-machine update timing (peer B held an in-flight pathing request)");
	}
	std::cout << Tag << " PASS two-peer-determinism" << std::endl;

	// (ii) a change at T is visible at T+H and not at T+H-1 on both peers.
	ResetHorizonWaitStats();
	PathFinder horizonA;
	PathFinder horizonB;
	horizonA.TestInstallGrid(8, 4, 20, &air);
	horizonB.TestInstallGrid(8, 4, 20, &air);
	const auto beforeA = pathCost(horizonA, true);
	const auto beforeB = pathCost(horizonB, true);
	horizonA.QueueHorizonDelta(20, 4, wall, blocked);
	horizonB.QueueHorizonDelta(20, 4, wall, blocked);
	horizonA.TestSetNodeMaterials(wall, blocked);
	horizonB.TestSetNodeMaterials(wall, blocked);
	horizonA.CommitHorizonThrough(23);
	horizonB.CommitHorizonThrough(23);
	const auto earlyA = pathCost(horizonA, true);
	const auto earlyB = pathCost(horizonB, true);
	if (earlyA != beforeA || earlyB != beforeB) {
		return fail("horizon change visible at T+H-1");
	}
	horizonA.CommitHorizonThrough(24);
	horizonB.CommitHorizonThrough(24);
	const auto lateA = pathCost(horizonA, true);
	const auto lateB = pathCost(horizonB, true);
	if (lateA == beforeA || lateB == beforeB || lateA != lateB) {
		return fail("horizon change not visible at T+H on both peers");
	}
	std::cout << Tag << " PASS horizon-visibility" << std::endl;

	// (iii) + production path: SLTerrain stamp, Scene::CalculatePath, AI vs shared at T+1.
	LoopbackTransport idle;
	NetLockstepConfig lockstep;
	lockstep.localPeerId = 1;
	lockstep.remotePeerId = 2;
	lockstep.peerCount = 2;
	lockstep.matchConfig = NetMatchConfigUtil::MakeDefault(0x5048413453455353ULL);
	lockstep.matchConfig.pathHorizonTicks = 4;
	NetLockstepCoordinator seated;
	std::string lockstepError;
	if (!seated.StartReplay(idle, lockstep, &lockstepError)) {
		return fail("lockstep replay did not start");
	}
	ScenarioRunner::SetLockstepCoordinator(&seated);
	ScenarioRunner::SetLockstepAppliedFrame(10);
	Scene scene;
	SLTerrain terrain;
	scene.TestSetTerrain(&terrain);
	scene.TestInstallHorizonPathFinders(8, 4, 20, &air);
	g_SceneMan.TestBindCurrentScene(&scene);
	if (g_SceneMan.GetScene() != &scene || scene.GetTerrain() != &terrain) {
		return fail("SceneMan test bind did not install the fixture scene and terrain");
	}
	const int sceneWall = scene.GetPathFinder(Activity::Teams::NoTeam).TestNodeIdAt(3, 2);
	if (sceneWall < 0) {
		return fail("scene fixture grid did not produce a wall node");
	}
	terrain.AddUpdatedMaterialArea(Box(Vector(50.0F, 30.0F), 40.0F, 40.0F));
	const float beforeShared = sceneCost(scene);
	scene.GetPathFinder(Activity::Teams::NoTeam).TestApplyLiveUpdate(sceneWall, blocked);
	ScenarioRunner::SetLockstepAppliedFrame(11);
	const float sharedAtT1 = sceneCost(scene);
	Actor aiActor;
	g_CurrentAIActor = &aiActor;
	const float aiAtT1 = sceneCost(scene);
	g_CurrentAIActor = nullptr;
	g_SceneMan.TestBindCurrentScene(nullptr);
	ScenarioRunner::SetLockstepCoordinator(nullptr);
	if (sharedAtT1 != beforeShared) {
		return fail("shared Scene::CalculatePath at T+1 did not stay on the origin-tick overlay");
	}
	if (aiAtT1 == beforeShared) {
		return fail("Scene::CalculatePath with a live Actor as g_CurrentAIActor did not see the live grid at T+1");
	}
	std::cout << Tag << " PASS ai-live-grid" << std::endl;
	std::cout << Tag << " PASS scene-production-path" << std::endl;

	// (iv) wait_us is zero when ready before wait, and the late fault is 50 ms plus slack.
	ResetHorizonWaitStats();
	PathFinder onTime;
	onTime.TestInstallGrid(8, 4, 20, &air);
	onTime.QueueHorizonDelta(1, 1, wall, blocked);
	onTime.CommitHorizonThrough(2);
	if (onTime.LastHorizonWaitUs() != 0) {
		std::cout << Tag << " FAIL horizon_wait_us was " << onTime.LastHorizonWaitUs() << " when the worker was on time" << std::endl;
		return 1;
	}
	TestArmFaultInject("horizon_worker_late");
	PathFinder late;
	late.TestInstallGrid(8, 4, 20, &air);
	late.QueueHorizonDelta(1, 1, wall, blocked);
	late.CommitHorizonThrough(2);
	TestArmFaultInject("");
	constexpr int64_t lateSleepUs = 50000;
	constexpr int64_t lateSlackUs = 25000;
	if (late.LastHorizonWaitUs() < lateSleepUs || late.LastHorizonWaitUs() > lateSleepUs + lateSlackUs) {
		std::cout << Tag << " FAIL late worker wait " << late.LastHorizonWaitUs() << " was not the injected 50 ms plus slack" << std::endl;
		return 1;
	}
	std::cout << Tag << " PASS stall-instrumentation" << std::endl;
	std::cout << Tag << " PASS" << std::endl;
	return 0;
}

void PathFinder::DebugRender(BITMAP* targetBitmap, const Vector& targetPos) const {
	for (int x = 0; x < m_GridWidth; ++x) {
		Vector startPos = (m_NodeGrid[ConvertCoordsToNodeId(x, 0)].Pos - m_Offset) - targetPos;
		Vector endPos = startPos + Vector(0.0F, m_NodeDimension * m_GridHeight);
		line(targetBitmap, startPos.GetX(), startPos.GetY(), endPos.GetX(), endPos.GetY(), g_BlackColor);
	}

	for (int y = 0; y < m_GridHeight; ++y) {
		Vector startPos = (m_NodeGrid[ConvertCoordsToNodeId(0, y)].Pos - m_Offset) - targetPos;
		Vector endPos = startPos + Vector(m_NodeDimension * m_GridWidth, 0.0F);
		line(targetBitmap, startPos.GetX(), startPos.GetY(), endPos.GetX(), endPos.GetY(), g_BlackColor);
	}
}
