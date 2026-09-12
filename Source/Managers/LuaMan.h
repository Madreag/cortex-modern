#pragma once

#include "Singleton.h"
#include "Entity.h"
#include "RTETools.h"
#include "PerformanceMan.h"

#include "BS_thread_pool.hpp"

#include <array>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#define g_LuaMan LuaMan::Instance()

struct lua_State;

namespace RTE {

	class LuabindObjectWrapper;
	class MovableObject;
	class Scene;
	struct PathRequest;
	struct LuaPathCallbackContext;

	/// A single lua state. Multiple of these can exist at once for multithreaded scripting.
	class LuaStateWrapper {
	public:
#pragma region Creation
		/// Constructor method used to instantiate a LuaStateWrapper object in system memory. Initialize() should be called before using the object.
		LuaStateWrapper();

		/// Makes the LuaStateWrapper object ready for use.
		void Initialize();
#pragma endregion

#pragma region Destruction
		/// Destructor method used to clean up a LuaStateWrapper object before deletion from system memory.
		~LuaStateWrapper();

		/// Destroys and resets (through Clear()) the LuaStateWrapper object.
		void Destroy();
#pragma endregion

#pragma region Getters and Setters
		/// Gets a temporary Entity that can be accessed in the Lua state.
		/// @return The temporary entity. Ownership is NOT transferred!
		Entity* GetTempEntity() const;

		/// Sets a temporary Entity that can be accessed in the Lua state.
		/// @param entity The temporary entity. Ownership is NOT transferred!
		void SetTempEntity(Entity* entity);

		/// Gets the temporary vector of Entities that can be accessed in the Lua state.
		/// @return The temporary vector of entities. Ownership is NOT transferred!
		const std::vector<Entity*>& GetTempEntityVector() const;

		/// Sets a temporary vector of Entities that can be accessed in the Lua state. These Entities are const_cast so they're non-const, for ease-of-use in Lua.
		/// @param entityVector The temporary vector of entities. Ownership is NOT transferred!
		void SetTempEntityVector(const std::vector<const Entity*>& entityVector);

		/// Sets the proper package.path for the script to run.
		/// @param filePath The path to the file to load and run.
		void SetLuaPath(const std::string& filePath);

		/// Gets this LuaStateWrapper's internal lua state.
		/// @return This LuaStateWrapper's internal lua state.
		lua_State* GetLuaState() { return m_State; };

		/// Seeds this state's RNG. Called per activity start so Lua math.random is reproducible.
		void SeedRandomGenerator(uint64_t seed);

		/// Serializes this state's RNG into a string for the per-tick lua_state checksum.
		std::string GetRandomGeneratorStateForHashing() const;

		/// Captures this state's random generator for restoration.
		std::string GetRandomGeneratorCheckpoint() const { return m_RandomGenerator.SerializeCheckpoint(); }

		/// Restores this state's random generator from a checkpoint.
		bool RestoreRandomGeneratorCheckpoint(std::string_view text) { return m_RandomGenerator.RestoreCheckpoint(text); }

		/// Gets m_ScriptTimings.
		/// @return m_ScriptTimings.
		const std::unordered_map<std::string, PerformanceMan::ScriptTiming>& GetScriptTimings() const;

		/// Gets the currently running script filepath, if applicable.
		/// @return The currently running script filepath. May return inaccurate values for non-MO scripts due to weirdness in setup.
		std::string_view GetCurrentlyRunningScriptFilePath() const { return m_CurrentlyRunningScriptPath; }
#pragma endregion

#pragma region Script Responsibility Handling
		/// Registers an MO as using us.
		/// @param moToRegister The MO to register with us. Ownership is NOT transferred!
		void RegisterMO(MovableObject* moToRegister) { m_AddedRegisteredMOs.insert(moToRegister); }

		/// Unregisters an MO as using us.
		/// @param moToUnregister The MO to unregister as using us. Ownership is NOT transferred!
		void UnregisterMO(MovableObject* moToUnregister) {
			m_RegisteredMOs.erase(moToUnregister);
			m_AddedRegisteredMOs.erase(moToUnregister);
		}

		/// A destroyed object leaves the lists a set-aside world will swap back; a detached live one stays.
		void ForgetDestroyedRegisteredMO(MovableObject* moToForget) {
			for (auto* held: m_HeldRegisteredMOs) {
				held->erase(moToForget);
			}
		}

		/// Gets a list of the MOs registed as using us.
		/// @return The MOs registed as using us.
		const std::unordered_set<MovableObject*>& GetRegisteredMOs() const { return m_RegisteredMOs; }
		/// Gets the objects waiting to join the script update list.
		const std::unordered_set<MovableObject*>& GetPendingRegisteredMOs() const { return m_AddedRegisteredMOs; }
		/// Swaps both script update lists between worlds while the simulation is stopped.
		void SwapRegisteredMOs(std::unordered_set<MovableObject*>& registered, std::unordered_set<MovableObject*>& pending) {
			m_RegisteredMOs.swap(registered);
			m_AddedRegisteredMOs.swap(pending);
		}
		/// Hands both lists to a set-aside world and marks them held, so nothing can be destroyed in between.
		void SwapAndHoldRegisteredMOs(std::unordered_set<MovableObject*>& registered, std::unordered_set<MovableObject*>& pending) {
			m_RegisteredMOs.swap(registered);
			m_AddedRegisteredMOs.swap(pending);
			m_HeldRegisteredMOs.push_back(&registered);
			m_HeldRegisteredMOs.push_back(&pending);
		}
		void ForgetHeldRegisteredMOs(std::unordered_set<MovableObject*>& registered, std::unordered_set<MovableObject*>& pending) {
			std::erase(m_HeldRegisteredMOs, &registered);
			std::erase(m_HeldRegisteredMOs, &pending);
		}
		/// The address of the object's Lua table (as a hex string), or "-" when it has none; the identity oracles compare it.
		std::string DescribeScriptObjectIdentity(long uniqueID);

		/// Installs the script graph codec into this state once.
		void LoadScriptGraphHelper();

		/// Runs the script graph's contract tests in this state and prints their lines; true when all pass.
		bool RunScriptGraphSelfTest();

		/// Records which globals and loaded modules the engine itself installed; the graph carries only what scripts added.
		void CaptureScriptGraphBaseline();

		/// Serializes the state's script graph: every scripted object's fields and every script-made global.
		/// @param problems Receives what could not be carried; any entry means the capture is not faithful.
		/// @return Whether the graph carries everything.
		bool SerializeScriptGraph(std::string& text, std::vector<std::string>& problems);

		/// The unique ids of the objects a graph text holds fields for.
		std::vector<long> ListScriptGraphRoots(const std::string& text);

		/// Prepares owned objects, or binds roots when text is null, before any state's graph is restored.
		bool PrepareScriptGraph(const std::string* text, std::vector<std::string>& problems, bool reuseHeld = false);

		/// Validates graph structure, bytecode and generator state without changing live objects.
		bool ValidateScriptGraph(const std::string& text, std::vector<std::string>& problems);

		/// Detaches the old VM's Lua-owned native identities before replacement objects adopt them.
		void ReleaseScriptOwnedObjects();
		bool HasNativeAliases(const std::unordered_set<const void*>& objects);
		bool RekeyScriptObjects(const std::vector<std::pair<const MovableObject*, long>>& identities, bool validateOnly = false);

		/// Restores a graph into this state, laying each root's fields onto its live object; false with the reasons when anything did not restore.
		bool RestoreScriptGraph(const std::string& text, std::vector<std::string>& problems, bool reuseHeld = false);

		/// Restores the per-object script fields carried by older saves.
		bool RestoreLegacyScriptObjectFields(long uniqueID, const std::string& text);

		/// Exposes cached functions and object callbacks to the script graph.
		void CaptureScriptCallbacks();

		/// Rebinds cached functions and object callbacks from the restored graph.
		void RestoreScriptCallbacks(std::vector<std::string>& problems, bool restoreAsync = true);

		/// Moves _ScriptedObjects[uid] into a stash so a stand-in self can take the slot; UnstashScriptObject puts it back.
		void StashScriptObject(long uniqueID);
		void UnstashScriptObject(long uniqueID);
		void DiscardStashedScriptObject(long uniqueID);

		/// Copies `_ScriptedObjects[uid]`'s instance into `_ScriptFieldsStash["preview:<uid>"]`.
		bool CopyScriptInstanceToPreviewHold(long uniqueID, std::vector<std::string>& problems);
		bool SnapshotPreviewGlobals(std::string& text, std::vector<std::string>& problems);
		bool RestorePreviewGlobals(const std::string& text, std::vector<std::string>& problems);
		bool BindPreviewScriptObject(MovableObject* clone, bool sharedSlot);
		void DropPreviewScriptObject(long uniqueID);
		bool AttachPreviewInvStride(MovableObject* object);

		/// Reads a number field off the object's self.
		/// @return The field, or the fallback when the object or the field is absent.
		double GetScriptObjectNumberField(long uniqueID, const std::string& field, double fallback);
#pragma endregion

#pragma region Script Execution Handling
		/// Runs the given Lua function with optional safety checks and arguments. The first argument to the function will always be the self object.
		/// If either argument list has entries, they will be passed into the function in order, with entity arguments first.
		/// @param functionName The name that gives access to the function in the global Lua namespace.
		/// @param selfObjectName The name that gives access to the self object in the global Lua namespace.
		/// @param variablesToSafetyCheck Optional vector of strings that should be safety checked in order before running the Lua function. Defaults to empty.
		/// @param functionEntityArguments Optional vector of entity pointers that should be passed into the Lua function. Their internal Lua states will not be accessible. Defaults to empty.
		/// @param functionLiteralArguments Optional vector of strings that should be passed into the Lua function. Entries must be surrounded with escaped quotes (i.e.`\"`) they'll be passed in as-is, allowing them to act as booleans, etc. Defaults to empty.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int RunScriptFunctionString(const std::string& functionName, const std::string& selfObjectName, const std::vector<std::string_view>& variablesToSafetyCheck = std::vector<std::string_view>(), const std::vector<const Entity*>& functionEntityArguments = std::vector<const Entity*>(), const std::vector<std::string_view>& functionLiteralArguments = std::vector<std::string_view>());

		/// Takes a string containing a script snippet and runs it on the state.
		/// @param scriptString The string with the script snippet.
		/// @param consoleErrors Whether to report any errors to the console immediately.
		/// @return Returns less than zero if any errors encountered when running this script. To get the actual error string, call GetLastError.
		int RunScriptString(const std::string& scriptString, bool consoleErrors = true);

		/// Runs the given Lua function object. The first argument to the function will always be the self object.
		/// If either argument list has entries, they will be passed into the function in order, with entity arguments first.
		/// @param functionObjectWrapper The LuabindObjectWrapper containing the Lua function to be run.
		/// @param selfGlobalTableName The name of the global Lua table that gives access to the self object.
		/// @param selfGlobalTableKey The key for this object in the respective global Lua table.
		/// @param functionEntityArguments Optional vector of entity pointers that should be passed into the Lua function. Their internal Lua states will not be accessible. Defaults to empty.
		/// @param functionLiteralArguments Optional vector of strings that should be passed into the Lua function. Entries must be surrounded with escaped quotes (i.e.`\"`) they'll be passed in as-is, allowing them to act as booleans, etc.. Defaults to empty.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int RunScriptFunctionObject(const LuabindObjectWrapper* functionObjectWrapper, const std::string& selfGlobalTableName, const std::string& selfGlobalTableKey, const std::vector<const Entity*>& functionEntityArguments = std::vector<const Entity*>(), const std::vector<std::string_view>& functionLiteralArguments = std::vector<std::string_view>(), const std::vector<LuabindObjectWrapper*>& functionObjectArguments = std::vector<LuabindObjectWrapper*>());
		
		/// Runs the given Lua function object. The first argument to the function will always be the self object.
		/// If either argument list has entries, they will be passed into the function in order, with entity arguments first.
		/// @param functionObjectWrapper The LuabindObjectWrapper containing the Lua function to be run.
		/// @param selfGlobalTableName The name of the global Lua table that gives access to the self object.
		/// @param selfGlobalTableKey The key for this object in the respective global Lua table.
		/// @param functionEntityArguments Optional vector of entity pointers that should be passed into the Lua function. Their internal Lua states will not be accessible. Defaults to empty.
		/// @param functionLiteralArguments Optional vector of strings that should be passed into the Lua function. Entries must be surrounded with escaped quotes (i.e.`\"`) they'll be passed in as-is, allowing them to act as booleans, etc.. Defaults to empty.
		/// @return An error return value signaling success or any particular failure. Anything below 0 is an error signal.
		int RunScriptConditionalTestFunctionObject(const LuabindObjectWrapper* functionObjectWrapper, const std::string& selfGlobalTableName, const std::string& selfGlobalTableKey, bool& returnParam, const std::vector<const Entity*>& functionEntityArguments = std::vector<const Entity*>(), const std::vector<std::string_view>& functionLiteralArguments = std::vector<std::string_view>(), const std::vector<LuabindObjectWrapper*>& functionObjectArguments = std::vector<LuabindObjectWrapper*>());

		/// Opens and loads a file containing a script and runs it on the state.
		/// @param filePath The path to the file to load and run.
		/// @param consoleErrors Whether to report any errors to the console immediately.
		/// @param doInSandboxedEnvironment Whether to do it in a sandboxed environment, or the global environment.
		/// @return Returns less than zero if any errors encountered when running this script. To get the actual error string, call GetLastError.
		int RunScriptFile(const std::string& filePath, bool consoleErrors = true, bool doInSandboxedEnvironment = true);

		/// Retrieves all of the specified functions that exist into the output map, and refreshes the cache.
		/// @param filePath The path to the file to load and run.
		/// @param functionNamesToLookFor The vector of strings defining the function names to be retrieved.
		/// @param outFunctionNamesAndObjects The map of function names to LuabindObjectWrappers to be retrieved from the script that was run.
		/// @return Returns whether functions were successfully retrieved.
		bool RetrieveFunctions(const std::string& functionObjectName, const std::vector<std::string>& functionNamesToLookFor, std::unordered_map<std::string, LuabindObjectWrapper*>& outFunctionNamesAndObjects);

		/// Opens and loads a file containing a script and runs it on the state, then retrieves all of the specified functions that exist into the output map.
		/// @param filePath The path to the file to load and run.
		/// @param functionNamesToLookFor The vector of strings defining the function names to be retrieved.
		/// @param outFunctionNamesAndObjects The map of function names to LuabindObjectWrappers to be retrieved from the script that was run.
		/// @param noCaching Whether caching shouldn't be used.
		/// @return Returns less than zero if any errors encountered when running this script. To get the actual error string, call GetLastError.
		int RunScriptFileAndRetrieveFunctions(const std::string& filePath, const std::vector<std::string>& functionNamesToLookFor, std::unordered_map<std::string, LuabindObjectWrapper*>& outFunctionNamesAndObjects, bool forceReload = false);
#pragma endregion

#pragma region Concrete Methods
		/// Updates this Lua state.
		void Update();

		/// Clears m_ScriptTimings.
		void ClearScriptTimings();
#pragma endregion

#pragma region MultiThreading
		/// Gets the mutex to lock this lua state.
		std::recursive_mutex& GetMutex() { return m_Mutex; };
#pragma endregion

#pragma region
		/// Gets whether the given Lua expression evaluates to true or false.
		/// @param expression The string with the expression to evaluate.
		/// @param consoleErrors Whether to report any errors to the console immediately.
		/// @return Whether the expression was true.
		bool ExpressionIsTrue(const std::string& expression, bool consoleErrors);

		/// Takes a pointer to an object and saves it in the Lua state as a global of a specified variable name.
		/// @param objectToSave The pointer to the object to save. Ownership is NOT transferred!
		/// @param globalName The name of the global var in the Lua state to save the pointer to.
		void SavePointerAsGlobal(void* objectToSave, const std::string& globalName);

		/// Checks if there is anything defined on a specific global var in Lua.
		/// @param globalName The name of the global var in the Lua state to check.
		/// @return Whether that global var has been defined yet in the Lua state.
		bool GlobalIsDefined(const std::string& globalName);

		/// Checks if there is anything defined in a specific index of a table.
		/// @param tableName The name of the table to look inside.
		/// @param indexName The name of the index to check inside that table.
		/// @return Whether that table var has been defined yet in the Lua state.
		bool TableEntryIsDefined(const std::string& tableName, const std::string& indexName);

		/// Clears internal Lua package tables from all user-defined modules. Those must be reloaded with ReloadAllScripts().
		void ClearUserModuleCache();

		/// Clears the Lua script cache.
		void ClearLuaScriptCache();
#pragma endregion

#pragma region Error Handling
		/// Tells whether there are any errors reported waiting to be read.
		/// @return Whether errors exist.
		bool ErrorExists() const;

		/// Returns the last error message from executing scripts.
		/// @return The error string with hopefully meaningful info about what went wrong.
		std::string GetLastError() const;

		/// Clears the last error message, so the Lua state will not be considered to have any errors until the next time there's a script error.
		void ClearErrors();
#pragma endregion

	private:
		/// Gets a random integer between minInclusive and maxInclusive.
		/// @return A random integer between minInclusive and maxInclusive.
		int SelectRand(int minInclusive, int maxInclusive);

		/// Gets a random real between minInclusive and maxInclusive.
		/// @return A random real between minInclusive and maxInclusive.
		double RangeRand(double minInclusive, double maxInclusive);

		/// Gets a random number between -1 and 1.
		/// @return A random number between -1 and 1.
		double NormalRand();

		/// Gets a random number between 0 and 1.
		/// @return A random number between 0 and 1.
		double PosRand();

#pragma region Passthrough LuaMan Functions
		const std::vector<std::string>* DirectoryList(const std::string& path);
		const std::vector<std::string>* FileList(const std::string& path);
		bool FileExists(const std::string& path);
		bool DirectoryExists(const std::string& path);
		int FileOpen(const std::string& path, const std::string& accessMode);
		void FileClose(int fileIndex);
		void FileCloseAll();
		bool FileRemove(const std::string& path);
		bool DirectoryCreate1(const std::string& path);
		bool DirectoryCreate2(const std::string& path, bool recursive);
		bool DirectoryRemove1(const std::string& path);
		bool DirectoryRemove2(const std::string& path, bool recursive);
		bool FileRename(const std::string& oldPath, const std::string& newPath);
		bool DirectoryRename(const std::string& oldPath, const std::string& newPath);
		std::string FileReadLine(int fileIndex);
		void FileWriteLine(int fileIndex, const std::string& line);
		bool FileEOF(int fileIndex);
#pragma endregion

		/// Generates a string that describes the current state of the Lua stack, for debugging purposes.
		/// @return A string that describes the current state of the Lua stack.
		std::string DescribeLuaStack();

		/// Clears all the member variables of this LuaStateWrapper, effectively resetting the members of this abstraction level only.
		void Clear();

		std::unordered_set<MovableObject*> m_RegisteredMOs; //!< The objects using our lua state.
		std::vector<std::unordered_set<MovableObject*>*> m_HeldRegisteredMOs; //!< Script update lists a set-aside world will swap back.
		std::unordered_set<MovableObject*> m_AddedRegisteredMOs; //!< The objects using our lua state that were recently added.

		lua_State* m_State;
		bool m_ScriptGraphHelperLoaded = false; //!< Whether the script graph codec has been installed in this state.
		Entity* m_TempEntity; //!< Temporary holder for an Entity object that we want to pass into the Lua state without fuss. Lets you export objects to lua easily.
		std::vector<Entity*> m_TempEntityVector; //!< Temporary holder for a vector of Entities that we want to pass into the Lua state without a fuss. Usually used to pass arguments to special Lua functions.
		std::string m_LastError; //!< Description of the last error that occurred in the script execution.
		std::string_view m_CurrentlyRunningScriptPath; //!< The currently running script filepath.

		// This mutex is more for safety, and with new script/AI architecture we shouldn't ever be locking on a mutex. As such we use this primarily to fire asserts.
		std::recursive_mutex m_Mutex; //!< Mutex to ensure multiple threads aren't running something in this lua state simultaneously.

		struct LuaScriptFunctionObjects {
			std::unordered_map<std::string, LuabindObjectWrapper*> functionNamesAndObjects;
		};
		std::unordered_map<std::string, LuaScriptFunctionObjects> m_ScriptCache;

		std::unordered_map<std::string, PerformanceMan::ScriptTiming> m_ScriptTimings; //!< Internal map of script timings.

		// For determinism, every Lua state has it's own random number generator.
		RandomGenerator m_RandomGenerator; //!< The random number generator used for this lua state.
	};

	typedef std::vector<LuaStateWrapper> LuaStatesArray;

	/// The singleton manager of each Lua state.
	class LuaMan : public Singleton<LuaMan> {
		friend class SettingsMan;
		friend class LuaStateWrapper;

	public:
#pragma region Creation
		/// Constructor method used to instantiate a LuaMan object in system memory. Initialize() should be called before using the object.
		LuaMan();

		/// Makes the LuaMan object ready for use.
		void Initialize();

		/// Scripts are frozen inside a rollback re-sim window; every Lua entry point returns without running.
		static bool AreScriptsFrozen() { return s_ScriptsFrozen; }
		static void SetScriptsFrozen(bool frozen) { s_ScriptsFrozen = frozen; }
		static inline bool s_ScriptsFrozen = false;

		/// Copies each original's self into a hold table. A refused self stays frozen.
		static void CapturePreviewSelfCopies(const std::vector<const MovableObject*>& roots, bool sharedSlot);
		/// Binds each clone to `_ScriptedObjects["<uid>#preview"]`, or the shared slot when sharedSlot is set.
		static void BeginPreviewScripts(const std::vector<MovableObject*>& clones, bool sharedSlot);
		/// Drops the preview slots.
		static void EndPreviewScripts();
		static bool IsPreviewClone(const MovableObject* mo);
		static bool IsPreviewEdgeHook(const std::string& functionName);
		static bool ShouldRunPreviewHook(const MovableObject* mo, const std::string& functionName);
		static bool IsRunningPreviewHook() { return s_RunningPreviewHook; }
		static void SetRunningPreviewHook(bool running) { s_RunningPreviewHook = running; }
		struct PreviewHookScope {
			const bool previous;
			explicit PreviewHookScope(bool on) :
			    previous(s_RunningPreviewHook) {
				s_RunningPreviewHook = previous || on;
			}
			~PreviewHookScope() { s_RunningPreviewHook = previous; }
			PreviewHookScope(const PreviewHookScope&) = delete;
			PreviewHookScope& operator=(const PreviewHookScope&) = delete;
		};
		static std::string PreviewScriptKey(const MovableObject* mo);
		static uint64_t PreviewCodecFallbackCount() { return s_PreviewCodecFallbacks; }
#pragma endregion

#pragma region Destruction
		/// Destructor method used to clean up a LuaMan object before deletion from system memory.
		~LuaMan();

		/// Destroys and resets (through Clear()) the LuaMan object.
		void Destroy();
#pragma endregion

#pragma region Lua State Handling
		/// Returns our master script state (where activies, global scripts etc run).
		/// @return The master script state.
		LuaStateWrapper& GetMasterScriptState();

		/// Returns our threaded script states which movable objects use.
		/// @return A list of threaded script states.
		LuaStatesArray& GetThreadedScriptStates();

		/// The save index of a state: 0 for the master state, 1 onwards for the threaded ones, -1 for none.
		int GetStateIndex(const LuaStateWrapper* state) const;

		/// Gets the threaded state cursor used by the next unassigned script.
		int GetScriptStateCursor() const { return m_LastAssignedLuaState; }

		/// Restores the threaded state cursor after a checkpoint.
		void SetScriptStateCursor(int cursor) { m_LastAssignedLuaState = m_ScriptStates.empty() ? 0 : cursor % m_ScriptStates.size(); }

		/// The state a save index names, wrapping when this machine has fewer threaded states.
		LuaStateWrapper& GetStateByIndex(int index);

		/// Drops a destroyed object from every held script update list, whichever state holds it.
		void ForgetDestroyedRegisteredMO(MovableObject* moToForget) {
			m_MasterScriptState.ForgetDestroyedRegisteredMO(moToForget);
			for (LuaStateWrapper& state: m_ScriptStates) {
				state.ForgetDestroyedRegisteredMO(moToForget);
			}
		}

		/// Runs the script graph's contract tests in the master state and prints their lines; true when all pass.
		bool RunScriptGraphSelfTest();

		/// Gets the current thread lua state override that new objects created will be assigned to.
		/// @return The current lua state to force objects to be assigned to.
		LuaStateWrapper* GetThreadLuaStateOverride() const;

		/// Forces all new MOs created in this thread to be assigned to a particular lua state.
		/// This is to ensure that objects created in threaded Lua environments can be safely used.
		/// @param luaState The lua state to force objects to be assigned to.
		void SetThreadLuaStateOverride(LuaStateWrapper* luaState);

		/// Gets the current thread lua state that is running.
		/// @return The current lua state that is running.
		LuaStateWrapper* GetThreadCurrentLuaState() const;

		/// Returns a free threaded script states to assign a movableobject to.
		/// This will be locked to our thread and safe to use - ensure that it'll be unlocked after use!
		/// @return A script state.
		LuaStateWrapper* GetAndLockFreeScriptState();

		/// Clears internal Lua package tables from all user-defined modules. Those must be reloaded with ReloadAllScripts().
		void ClearUserModuleCache();

		/// Gets the callback queue for the current world.
		std::shared_ptr<LuaPathCallbackContext> GetPathCallbackContext() const { return m_PathCallbacks; }

		/// Allocates a callback ID within one Lua state and world.
		static int AllocatePathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state);

		/// Registers a path request before dispatching its asynchronous work.
		static void StartPathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state, int id, Scene* scene, const Vector& start, const Vector& end, float jumpHeight, float digStrength, int team);

		/// Queues an immutable path result for delivery on the main thread.
		static void CompletePathCallback(const std::shared_ptr<LuaPathCallbackContext>& context, lua_State* state, int id, const PathRequest& result);

		/// Starts a fresh callback queue, optionally releasing old Lua closures.
		void ResetPathCallbacks(bool clearLua = false);

		/// Swaps the live world's callback queue with a held world.
		void SwapPathCallbacks(std::shared_ptr<LuaPathCallbackContext>& context);

		/// Captures one consistent queue view for all Lua states.
		void BeginPathCallbackCapture();
		void EndPathCallbackCapture();

		/// Pushes a Lua state's queued callbacks and allocation cursor.
		void PushPathCallbacks(lua_State* state);

		/// Restores a Lua state's queue from the table at index.
		bool RestorePathCallbacks(lua_State* state, int index);

		/// Resubmits pending requests after their Lua graphs have been restored.
		void ResumePathCallbacks();

		/// Executes and clears all pending script callbacks.
		void ExecuteLuaScriptCallbacks();

		/// Gets m_ScriptTimings.
		/// @return m_ScriptTimings.
		const std::unordered_map<std::string, PerformanceMan::ScriptTiming> GetScriptTimings() const;
#pragma endregion

#pragma region File I/O Handling
		/// Returns a vector of all the directories in path, which is relative to the working directory.
		/// @param path Directory path relative to the working directory.
		/// @return A vector of the directories in path.
		const std::vector<std::string>* DirectoryList(const std::string& path);

		/// Returns a vector of all the files in path, which is relative to the working directory.
		/// @param path Directory path relative to the working directory.
		/// @return A vector of the files in path.
		const std::vector<std::string>* FileList(const std::string& path);

		/// Returns whether or not the specified file exists. You can only check for files inside .rte folders in the working directory.
		/// @param path Path to the file. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not the specified file exists.
		bool FileExists(const std::string& path);

		/// Returns whether or not the specified directory exists. You can only check for directories inside .rte folders in the working directory.
		/// @param path Path to the directory. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not the specified file exists.
		bool DirectoryExists(const std::string& path);

		/// Returns whether or not the path refers to an accessible file or directory. You can only check for files or directories inside .rte directories in the working directory.
		/// @param path Path to the file or directory. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not the specified file exists.
		bool IsValidModulePath(const std::string& path);

		/// Opens a file or creates one if it does not exist, depending on access mode. You can open files only inside .rte folders in the working directory. You can't open more that c_MaxOpenFiles file simultaneously.
		/// On Linux will attempt to open a file case insensitively.
		/// @param path Path to the file. All paths are made absolute by adding current working directory to the specified path.
		/// @param mode File access mode. See 'fopen' for list of modes.
		/// @return File index in the opened files array.
		int FileOpen(const std::string& path, const std::string& accessMode);

		/// Closes a previously opened file.
		/// @param fileIndex File index in the opened files array.
		void FileClose(int fileIndex);

		/// Closes all previously opened files.
		void FileCloseAll();

		/// Removes a file.
		/// @param path Path to the file. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not the file was removed.
		bool FileRemove(const std::string& path);

		/// Creates a directory, optionally recursively.
		/// @param path Path to the directory to be created. All paths are made absolute by adding current working directory to the specified path.
		/// @param recursive Whether to recursively create parent directories.
		/// @return Whether or not the directory was removed.
		bool DirectoryCreate(const std::string& path, bool recursive);

		/// Removes a directory, optionally recursively.
		/// @param path Path to the directory to be removed. All paths are made absolute by adding current working directory to the specified path.
		/// @param recursive Whether to recursively remove files and directories.
		/// @return Whether or not the directory was removed.
		bool DirectoryRemove(const std::string& path, bool recursive);

		/// Moves or renames the file oldPath to newPath.
		/// In order to get consistent behavior across Windows and Linux across all 4 combinations of oldPath and newPath being a directory/file,
		/// the newPath isn't allowed to already exist.
		/// @param oldPath Path to the filesystem object. All paths are made absolute by adding current working directory to the specified path.
		/// @param newPath Path to the filesystem object. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not renaming succeeded.
		bool FileRename(const std::string& oldPath, const std::string& newPath);

		/// Moves or renames the directory oldPath to newPath.
		/// In order to get consistent behavior across Windows and Linux across all 4 combinations of oldPath and newPath being a directory/file,
		/// the newPath isn't allowed to already exist.
		/// @param oldPath Path to the filesystem object. All paths are made absolute by adding current working directory to the specified path.
		/// @param newPath Path to the filesystem object. All paths are made absolute by adding current working directory to the specified path.
		/// @return Whether or not renaming succeeded.
		bool DirectoryRename(const std::string& oldPath, const std::string& newPath);

		/// Reads a line from a file.
		/// @param fileIndex File index in the opened files array.
		/// @return Line from file, or empty string on error.
		std::string FileReadLine(int fileIndex);

		/// Writes a text line to a file.
		/// @param fileIndex File index in the opened files array.
		/// @param line String to write.
		void FileWriteLine(int fileIndex, const std::string& line);

		/// Returns true if end of file was reached.
		/// @param fileIndex File index in the opened files array.
		/// @return Whether or not EOF was reached.
		bool FileEOF(int fileIndex);
#pragma endregion

#pragma region Concrete Methods
		/// Updates the state of this LuaMan.
		void Update();

		/// Asynchronously enforces a GC run to occur.
		void StartAsyncGarbageCollection();

		/// Sets whether every state's tick-end collection is a full cycle, as a run that must agree with another run needs, or the incremental step.
		/// @param deterministic Whether every tick end runs a full collection on every state.
		static void SetDeterministicCollection(bool deterministic);

		/// Gets whether every state's tick-end collection is a full cycle.
		/// @return Whether every tick end runs a full collection on every state.
		static bool IsDeterministicCollection();

		/// Reseeds every Lua state's RNG, deriving an independent per-state seed from baseSeed.
		void SeedAllLuaRNGs(uint64_t baseSeed);

		/// Folds the master Lua state's RNG, then every scripted object's script graph in unique ID order, into the lua_state SimChecksum subsystem.
		void HashAllLuaStatesIntoSimChecksum();

		/// Blocks until any in-flight async GC finishes.
		void WaitForAsyncGarbageCollection();

		/// Collects every state fully so a checkpoint captures a settled object graph.
		void CollectGarbageForCheckpoint();
#pragma endregion

		/// Clears Script Timings.
		void ClearScriptTimings();

	private:
		static constexpr int c_MaxOpenFiles = 10; //!< The maximum number of files that can be opened with FileOpen at runtime.
		static const std::unordered_set<std::string> c_FileAccessModes; //!< Valid file access modes when opening files with FileOpen.

		std::array<FILE*, c_MaxOpenFiles> m_OpenedFiles; //!< Internal list of opened files used by File functions.

		LuaStateWrapper m_MasterScriptState;
		LuaStatesArray m_ScriptStates;

		std::shared_ptr<LuaPathCallbackContext> m_PathCallbacks; //!< The current world's asynchronous callbacks.
		std::shared_ptr<LuaPathCallbackContext> m_PathCallbackCapture; //!< The queue view used during graph capture.

		int m_LastAssignedLuaState = 0;

		BS::multi_future<void> m_GarbageCollectionTask;

		/// Clears all the member variables of this LuaMan, effectively resetting the members of this abstraction level only.
		void Clear();

		// Disallow the use of some implicit methods.
		LuaMan(const LuaMan& reference) = delete;
		LuaMan& operator=(const LuaMan& rhs) = delete;

		static inline bool s_RunningPreviewHook = false;
		static inline bool s_PreviewSharedSlot = false;
		static inline uint64_t s_PreviewCodecFallbacks = 0;
		static std::unordered_set<const MovableObject*> s_PreviewClones;
		static std::unordered_set<long> s_PreviewFrozenUIDs;
		static std::vector<std::pair<LuaStateWrapper*, std::string>> s_PreviewGlobalSnapshots;
	};

	/// RAII redirect of the C++ sim-RNG free functions and Lua math.random to one
	/// per-MO generator seeded from (uniqueID, sim tick, phase), so threaded per-MO
	/// work draws a stream that depends only on the MO and the tick.
	class DeterministicMORNGScope {
	public:
		/// @param uniqueID The MovableObject's GetUniqueID().
		/// @param phase Per-hook salt so an MO's different hooks don't correlate.
		/// @param enabled When false the scope is a no-op (leaves collision callbacks on the per-state RNG).
		DeterministicMORNGScope(long uniqueID, uint64_t phase, bool enabled = true);
		~DeterministicMORNGScope();

		DeterministicMORNGScope(const DeterministicMORNGScope&) = delete;
		DeterministicMORNGScope& operator=(const DeterministicMORNGScope&) = delete;

	private:
		bool m_Installed;
		RandomGenerator* m_PrevSimOverride;
		RandomGenerator* m_PrevLuaOverride;
	};
} // namespace RTE
