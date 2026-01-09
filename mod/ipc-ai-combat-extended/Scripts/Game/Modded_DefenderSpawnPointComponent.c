//------------------------------------------------------------------------------------------------
// IPC AI Combat Extended - Modded Defender Spawn Component
// Extends base IPC mod to add reinforcement behavior to defending enemy spawns
//
// Base behavior: 180s respawn time, 2 groups per spawn point
// Reinforcement behavior: When players attack this base for 5+ minutes,
//                         spawn larger reinforcement waves with increased spawn dispersion
//------------------------------------------------------------------------------------------------

modded class IPC_DefenderSpawnPointComponent : IPC_SpawnPointComponent
{
	//------------------------------------------------------------------------------------------------
	// REINFORCEMENT TRACKING
	//------------------------------------------------------------------------------------------------

	protected WorldTimestamp m_tCombatStartTime;			// When combat started at this base
	protected bool m_bReinforcementActive = false;			// Is reinforcement mode active
	protected int m_iReinforcementWave = 0;					// Current wave number (0=none, 1=first, 2=second)
	protected WorldTimestamp m_tLastReinforcementTime;		// When last reinforcement spawned
	protected bool m_bIsReinforcementCoordinator = false;	// Is this spawn point the coordinator for this base?
	protected bool m_bCoordinatorInitialized = false;		// Has coordinator selection been done?

	// Reinforcement group tracking (for cleanup)
	protected ref array<SCR_AIGroup> m_aReinforcementGroups = new array<SCR_AIGroup>();

	// Reinforcement configuration
	protected const int REINFORCEMENT_WAVE1_THRESHOLD = 300;	// 5 minutes
	protected const int REINFORCEMENT_WAVE2_THRESHOLD = 600;	// 10 minutes
	protected const float COMBAT_DETECTION_RANGE = 300.0;		// Distance to detect player activity
	protected const int CHECK_INTERVAL = 30000;					// Check every 30 seconds (not 5!)

	// Normal spawn parameters
	protected const int NORMAL_RESPAWN_TIME = 180;
	protected const int NORMAL_GROUP_COUNT = 2;

	// Reinforcement spawn parameters
	protected const int REINFORCEMENT_GROUP_COUNT = 1;			// Number of reinforcement groups per wave
	protected const float REINFORCEMENT_SPAWN_RADIUS = 200.0;	// Spawn dispersion radius (100m base + 200m spread = 100-300m total)

	//------------------------------------------------------------------------------------------------
	//! Override initialization to set custom spawn parameters
	//------------------------------------------------------------------------------------------------
	override void EOnInit(IEntity owner)
	{
		super.EOnInit(owner);

		// Set normal spawn parameters
		m_iRespawnPeriod = NORMAL_RESPAWN_TIME;
		m_iNum = NORMAL_GROUP_COUNT;

		Print("[IPC Extended] Defender spawn point with reinforcement capability initialized", LogLevel.NORMAL);

		// Don't start coordinator selection here - m_nearBase isn't set yet
		// Will be triggered after first PrepareBase() call
	}

	//------------------------------------------------------------------------------------------------
	//! Override PrepareBase to trigger coordinator initialization after base is known
	//------------------------------------------------------------------------------------------------
	override void PrepareBase()
	{
		super.PrepareBase();

		// After base is prepared, initialize coordinator role if not done yet
		if (m_nearBase && !m_bCoordinatorInitialized)
		{
			m_bCoordinatorInitialized = true;  // Prevent multiple initialization attempts
			// Use a delay to let other spawn points also call PrepareBase
			GetGame().GetCallqueue().CallLater(InitializeReinforcementCoordinator, 5000, false);
			PrintFormat("[IPC Reinforcement] Scheduled coordinator initialization for spawn point at %1", m_nearBase.GetOwner().GetName());
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Initialize reinforcement coordinator role (called after delay to allow other spawn points to register)
	//------------------------------------------------------------------------------------------------
	protected void InitializeReinforcementCoordinator()
	{
		if (!m_nearBase)
			return;

		// Check if we're the first defender spawn point for this base
		// We use the entity ID to ensure consistent coordinator selection
		IPC_AutonomousCaptureSystem autonomousSystem = IPC_AutonomousCaptureSystem.GetInstance();
		if (!autonomousSystem)
			return;

		// Get all registered spawn points
		array<IPC_SpawnPointComponent> allSpawnPoints = {};
		autonomousSystem.GetPatrols(allSpawnPoints);
		if (!allSpawnPoints || allSpawnPoints.IsEmpty())
			return;

		int myEntityId = GetOwner().GetID();
		int lowestIdForBase = myEntityId;

		// Find the spawn point with lowest ID at this base
		foreach (IPC_SpawnPointComponent spawnPoint : allSpawnPoints)
		{
			IPC_DefenderSpawnPointComponent defenderPoint = IPC_DefenderSpawnPointComponent.Cast(spawnPoint);
			if (!defenderPoint)
				continue;

			// Check if same base
			if (defenderPoint.GetNearBase() != m_nearBase)
				continue;

			int theirId = defenderPoint.GetOwner().GetID();
			if (theirId < lowestIdForBase)
				lowestIdForBase = theirId;
		}

		// We're the coordinator if we have the lowest ID
		m_bIsReinforcementCoordinator = (myEntityId == lowestIdForBase);

		if (m_bIsReinforcementCoordinator)
		{
			PrintFormat("[IPC Reinforcement] Spawn point %1 is COORDINATOR for base %2",
						GetOwner().GetName(), m_nearBase.GetOwner().GetName());

			// Only the coordinator runs periodic checks (every 30 seconds)
			GetGame().GetCallqueue().CallLater(CheckReinforcements, CHECK_INTERVAL, true);
		}
		else
		{
			PrintFormat("[IPC Reinforcement] Spawn point %1 is NON-COORDINATOR for base %2 (no periodic checks)",
						GetOwner().GetName(), m_nearBase.GetOwner().GetName());
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Get the nearby base (for coordinator detection)
	//------------------------------------------------------------------------------------------------
	SCR_CampaignMilitaryBaseComponent GetNearBase()
	{
		return m_nearBase;
	}

	//------------------------------------------------------------------------------------------------
	//! Periodic check for reinforcements (called every 30 seconds, ONLY by coordinator)
	//------------------------------------------------------------------------------------------------
	protected void CheckReinforcements()
	{
		// Safety check: Only coordinators should run this
		if (!m_bIsReinforcementCoordinator)
			return;

		// Only check reinforcement logic if we have a nearby base to defend
		if (!m_nearBase)
			return;

		// Check if players are actively attacking this base
		bool combatActive = DetectCombatAtBase();

		// Update reinforcement state based on combat duration
		UpdateReinforcementState(combatActive);

		// Cleanup dead reinforcement groups
		CleanupDeadReinforcementGroups();
	}

	//------------------------------------------------------------------------------------------------
	//! Detect if players are attacking this defended base
	//------------------------------------------------------------------------------------------------
	protected bool DetectCombatAtBase()
	{
		if (!m_nearBase)
			return false;

		// Check if base is enemy-controlled (we're defending it)
		Faction baseFaction = m_nearBase.GetFaction();
		if (!baseFaction || baseFaction != m_Faction)
			return false; // Base captured or wrong faction

		// Check for nearby enemy players
		vector basePos = m_nearBase.GetOwner().GetOrigin();

		PlayerManager playerManager = GetGame().GetPlayerManager();
		if (!playerManager)
			return false;

		array<int> playerIds = {};
		playerManager.GetPlayers(playerIds);

		foreach (int playerId : playerIds)
		{
			IEntity player = playerManager.GetPlayerControlledEntity(playerId);
			if (!player)
				continue;

			// Check if player is alive and within combat range
			CharacterControllerComponent controller = CharacterControllerComponent.Cast(player.FindComponent(CharacterControllerComponent));
			if (!controller || controller.IsDead())
				continue;

			// Check if player is enemy faction
			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(player);
			if (!character)
				continue;

			Faction playerFaction = character.GetFaction();
			if (!playerFaction || playerFaction == m_Faction)
				continue; // Same faction, not an attacker

			float distSq = vector.DistanceSq(player.GetOrigin(), basePos);
			if (distSq < COMBAT_DETECTION_RANGE * COMBAT_DETECTION_RANGE)
				return true; // Enemy player attacking our base
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Update reinforcement state based on combat activity
	//------------------------------------------------------------------------------------------------
	protected void UpdateReinforcementState(bool combatActive)
	{
		ChimeraWorld world = GetOwner().GetWorld();
		if (!world)
			return;

		WorldTimestamp currentTime = world.GetServerTimestamp();

		// Start tracking combat
		if (combatActive && !m_bReinforcementActive)
		{
			m_tCombatStartTime = currentTime;
			m_bReinforcementActive = true;
			PrintFormat("[IPC Reinforcement] Combat detected at %1 - tracking started", m_nearBase.GetOwner().GetName());
		}

		// Reset if combat stopped
		if (!combatActive && m_bReinforcementActive)
		{
			ResetReinforcementState();
			PrintFormat("[IPC Reinforcement] Combat ended at %1 - reset", m_nearBase.GetOwner().GetName());
			return;
		}

		// Check if reinforcement threshold reached
		if (m_bReinforcementActive)
		{
			float combatDuration = currentTime.DiffMilliseconds(m_tCombatStartTime) / 1000.0;

			// Prevent re-triggering within cooldown period
			if (m_tLastReinforcementTime)
			{
				float timeSinceLastReinforcement = currentTime.DiffMilliseconds(m_tLastReinforcementTime) / 1000.0;
				if (timeSinceLastReinforcement < 10.0)
					return; // Still in cooldown
			}

			// Wave 2: 10 minutes
			if (m_iReinforcementWave < 2 && combatDuration >= REINFORCEMENT_WAVE2_THRESHOLD)
			{
				TriggerReinforcements(2);
			}
			// Wave 1: 5 minutes
			else if (m_iReinforcementWave < 1 && combatDuration >= REINFORCEMENT_WAVE1_THRESHOLD)
			{
				TriggerReinforcements(1);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Trigger reinforcement wave - manually spawn additional units independent of parent mod
	//------------------------------------------------------------------------------------------------
	protected void TriggerReinforcements(int wave)
	{
		ChimeraWorld world = GetOwner().GetWorld();
		if (!world)
		{
			Print("[IPC Reinforcement] ERROR: No world in TriggerReinforcements", LogLevel.ERROR);
			return;
		}

		m_tLastReinforcementTime = world.GetServerTimestamp();
		m_iReinforcementWave = wave;

		string baseName = m_nearBase.GetOwner().GetName();

		// Determine group type based on wave
		SCR_EGroupType groupType;
		if (wave == 1)
			groupType = SCR_EGroupType.FIRETEAM;
		else
			groupType = SCR_EGroupType.SQUAD_RIFLE;

		PrintFormat("[IPC Reinforcement] WAVE %1 triggering at %2 - spawning %3 reinforcement groups (type: %4)",
					wave, baseName, REINFORCEMENT_GROUP_COUNT, typename.EnumToString(SCR_EGroupType, groupType));

		// Spawn reinforcement groups manually
		int successfulSpawns = 0;
		for (int i = 0; i < REINFORCEMENT_GROUP_COUNT; i++)
		{
			SCR_AIGroup reinforcementGroup = SpawnReinforcementGroup(groupType);
			if (reinforcementGroup)
			{
				m_aReinforcementGroups.Insert(reinforcementGroup);
				successfulSpawns++;
			}
		}

		if (successfulSpawns > 0)
		{
			PrintFormat("[IPC Reinforcement] Successfully spawned %1/%2 reinforcement groups at %3",
						successfulSpawns, REINFORCEMENT_GROUP_COUNT, baseName);

			// Broadcast notification
			BroadcastReinforcementAlert(baseName, wave);
		}
		else
		{
			PrintFormat("[IPC Reinforcement] ERROR: Failed to spawn any reinforcement groups at %1", baseName);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Manually spawn a single reinforcement group (similar to parent mod's attacking units)
	//------------------------------------------------------------------------------------------------
	protected SCR_AIGroup SpawnReinforcementGroup(SCR_EGroupType groupType)
	{
		// Validate prerequisites
		if (m_sPrefab.IsEmpty())
		{
			Print("[IPC Reinforcement] ERROR: No group prefab defined", LogLevel.ERROR);
			return null;
		}

		if (!m_Faction)
		{
			Print("[IPC Reinforcement] ERROR: No faction defined", LogLevel.ERROR);
			return null;
		}

		if (!m_nearBase)
		{
			Print("[IPC Reinforcement] ERROR: No base reference", LogLevel.ERROR);
			return null;
		}

		// Load group prefab
		Resource prefab = Resource.Load(m_sPrefab);
		if (!prefab || !prefab.IsValid())
		{
			PrintFormat("[IPC Reinforcement] ERROR: Failed to load group prefab: %1", m_sPrefab);
			return null;
		}

		// Find spawn position near the base with dispersion
		vector basePos = m_nearBase.GetOwner().GetOrigin();
		vector spawnPos;

		// Use wider dispersion for reinforcements (100-300m from base)
		array<vector> positions = {};
		if (SCR_WorldTools.FindAllEmptyTerrainPositions(positions, basePos, REINFORCEMENT_SPAWN_RADIUS, 5, 2) > 0)
			spawnPos = positions.GetRandomElement();
		else
			spawnPos = basePos; // Fallback to base position

		// Setup spawn parameters
		EntitySpawnParams params = EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = spawnPos;

		// Spawn the group entity
		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(prefab, null, params));
		if (!group)
		{
			Print("[IPC Reinforcement] ERROR: Failed to spawn group entity", LogLevel.ERROR);
			return null;
		}

		// Spawn units within the group (REINFORCEMENT_GROUP_COUNT times)
		if (!group.GetSpawnImmediately())
		{
			for (int i = 0; i < REINFORCEMENT_GROUP_COUNT; i++)
			{
				group.SpawnUnits();
			}
		}

		// Configure AI agents
		array<AIAgent> agents = {};
		group.GetAgents(agents);
		group.PreventMaxLOD();

		foreach (AIAgent agent : agents)
		{
			agent.PreventMaxLOD();

			// Get combat component for AI configuration
			IEntity agentEntity = agent.GetControlledEntity();
			if (!agentEntity)
				continue;

			SCR_AIInfoComponent infoComponent = SCR_AIInfoComponent.Cast(agentEntity.FindComponent(SCR_AIInfoComponent));
			if (!infoComponent)
				continue;

			SCR_AICombatComponent combatComponent = infoComponent.GetCombatComponent();
			if (!combatComponent)
				continue;

			// Set AI skill based on player count (same as parent mod)
			int players = GetGame().GetPlayerManager().GetPlayerCount();
			if (players < 5)
			{
				combatComponent.SetAISkill(EAISkill.EXPERT);
				combatComponent.SetPerceptionFactor(1.5);
			}
			else if (players < 10)
			{
				combatComponent.SetAISkill(EAISkill.EXPERT);
				combatComponent.SetPerceptionFactor(1.5);
			}
			else
			{
				combatComponent.SetAISkill(EAISkill.CYLON);
				combatComponent.SetPerceptionFactor(2.0);
			}
		}

		// Create defend waypoint at base position
		CreateDefendWaypoint(group, basePos);

		PrintFormat("[IPC Reinforcement] Spawned reinforcement group with %1 agents (type: %2)",
					agents.Count(), typename.EnumToString(SCR_EGroupType, groupType));

		return group;
	}

	//------------------------------------------------------------------------------------------------
	//! Create a defend waypoint for a reinforcement group
	//------------------------------------------------------------------------------------------------
	protected void CreateDefendWaypoint(SCR_AIGroup group, vector targetPos)
	{
		if (!group)
			return;

		// Get waypoint prefab from component class data
		IPC_DefenderSpawnPointComponentClass componentData = IPC_DefenderSpawnPointComponentClass.Cast(GetComponentData(GetOwner()));
		if (!componentData)
		{
			Print("[IPC Reinforcement] WARNING: No component data for waypoint", LogLevel.WARNING);
			return;
		}

		Resource waypointResource = Resource.Load(componentData.GetDefaultWaypointPrefab());
		if (!waypointResource || !waypointResource.IsValid())
		{
			Print("[IPC Reinforcement] WARNING: Invalid waypoint prefab", LogLevel.WARNING);
			return;
		}

		// Find position near base for waypoint
		vector waypointPos;
		SCR_WorldTools.FindEmptyTerrainPosition(waypointPos, targetPos, 30, 2, 2);

		// Setup waypoint spawn parameters
		EntitySpawnParams params = EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = waypointPos;

		// Spawn and assign waypoint
		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(waypointResource, null, params));
		if (waypoint)
		{
			// Clear any existing waypoints
			array<AIWaypoint> existingWaypoints = {};
			group.GetWaypoints(existingWaypoints);
			foreach (AIWaypoint wp : existingWaypoints)
			{
				group.RemoveWaypoint(wp);
			}

			// Add defend waypoint
			group.AddWaypoint(waypoint);
			PrintFormat("[IPC Reinforcement] Created defend waypoint for reinforcement group at %1",
						m_nearBase.GetOwner().GetName());
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Broadcast reinforcement alert to all players
	//------------------------------------------------------------------------------------------------
	protected void BroadcastReinforcementAlert(string baseName, int wave)
	{
		// Schedule notification slightly delayed to avoid RPC timing issues
		GetGame().GetCallqueue().CallLater(DoSendReinforcementAlert, 100, false, baseName, wave);
	}

	//------------------------------------------------------------------------------------------------
	//! Actually send the reinforcement alert (called after short delay)
	//------------------------------------------------------------------------------------------------
	protected void DoSendReinforcementAlert(string baseName, int wave)
	{
		// Use SCR_PopUpNotification directly instead of RPC system
		// This avoids "RpcError: Calling a RPC from an unregistered item" error
		SCR_PopUpNotification popupSystem = SCR_PopUpNotification.GetInstance();
		if (!popupSystem)
		{
			Print("[IPC Reinforcement] Failed to get PopUpNotification system");
			return;
		}

		string title = "Enemy Reinforcements Detected";
		string subtitle = string.Format("AO: %1", baseName);

		// Show notification to all players (duration: 5 seconds)
		popupSystem.PopupMsg(title, 5.0, subtitle);

		Print("[IPC Reinforcement] Sent reinforcement notification to all players");
	}

	//------------------------------------------------------------------------------------------------
	//! Cleanup dead reinforcement groups (called periodically)
	//------------------------------------------------------------------------------------------------
	protected void CleanupDeadReinforcementGroups()
	{
		if (m_aReinforcementGroups.IsEmpty())
			return;

		// Check each reinforcement group
		for (int i = m_aReinforcementGroups.Count() - 1; i >= 0; i--)
		{
			SCR_AIGroup group = m_aReinforcementGroups[i];
			if (!group || group.GetAgentsCount() == 0)
			{
				// Group is dead or invalid, remove from tracking
				if (group)
				{
					PrintFormat("[IPC Reinforcement] Reinforcement group eliminated at %1",
								m_nearBase.GetOwner().GetName());
					// Note: Group entities auto-cleanup when all agents dead
				}
				m_aReinforcementGroups.Remove(i);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Reset reinforcement tracking
	//------------------------------------------------------------------------------------------------
	protected void ResetReinforcementState()
	{
		m_bReinforcementActive = false;
		m_iReinforcementWave = 0;

		// Cleanup is handled by CleanupDeadReinforcementGroups() periodic check
		// Groups will despawn naturally when all agents die
	}

	//------------------------------------------------------------------------------------------------
	//! Destructor - cleanup scheduled callbacks and reinforcement groups
	//------------------------------------------------------------------------------------------------
	void ~IPC_DefenderSpawnPointComponent()
	{
		// Remove the periodic check callback if we're the coordinator
		if (m_bIsReinforcementCoordinator)
		{
			GetGame().GetCallqueue().Remove(CheckReinforcements);
			PrintFormat("[IPC Reinforcement] Cleaned up coordinator callbacks for %1", GetOwner().GetName());
		}

		// Note: Reinforcement groups (m_aReinforcementGroups) will be automatically cleaned up
		// by Enfusion's garbage collector when this component is destroyed
		// Individual group entities auto-despawn when all agents die
	}
}
