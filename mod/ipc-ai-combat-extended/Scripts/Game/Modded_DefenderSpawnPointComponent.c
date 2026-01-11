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

	// Helicopter tracking (for cleanup)
	protected ref array<IEntity> m_aReinforcementHelicopters = new array<IEntity>();

	// Frontline detection for auto-despawn
	protected WorldTimestamp m_tInactiveSince;					// When base became inactive
	protected const int INACTIVE_GRACE_PERIOD = 600;			// 10 minutes before despawn
	protected const float FRONTLINE_RANGE = 2000.0;				// Enemy base within 5km = frontline

	// Reinforcement configuration
	protected const int REINFORCEMENT_WAVE1_THRESHOLD = 300;	// 5 minutes
	protected const int REINFORCEMENT_WAVE2_THRESHOLD = 600;	// 10 minutes
	protected const int REINFORCEMENT_WAVE3_THRESHOLD = 900;	// 15 minutes
	protected const int REINFORCEMENT_WAVE4_THRESHOLD = 1200;	// 20 minutes
	protected const float COMBAT_DETECTION_RANGE = 300.0;		// Distance to detect player activity
	protected const int CHECK_INTERVAL = 30000;					// Check every 30 seconds (not 5!)

	// Normal spawn parameters
	protected const int NORMAL_RESPAWN_TIME = 180;
	protected const int NORMAL_GROUP_COUNT = 2;

	// Reinforcement spawn parameters
	protected const int REINFORCEMENT_GROUP_COUNT = 1;			// Number of reinforcement groups per wave
	protected const float REINFORCEMENT_SPAWN_RADIUS = 200.0;	// Spawn dispersion radius (100m base + 200m spread = 100-300m total)

	// Helicopter configuration
	protected const string HELICOPTER_PREFAB_MI8MT = "{3C6B3ED0C3AC30D5}Prefabs/Vehicles/Helicopters/Mi8MT/Mi8MT_armed_gunship_HE.et";
	protected const float HELICOPTER_SPAWN_DISTANCE = 1500.0;	// Distance from base to spawn helicopter (1.5km)
	protected const float HELICOPTER_SPAWN_ALTITUDE = 200.0;	// Altitude above terrain to spawn helicopter

	// Debug mode configuration - Set to true for fast testing, false for normal gameplay
	protected const bool DEBUG_MODE = false;					// CHANGE TO true FOR TESTING
	protected const int DEBUG_WAVE_INTERVAL = 60;				// 1 minute intervals in debug mode

	//------------------------------------------------------------------------------------------------
	//! Override initialization to set custom spawn parameters
	//------------------------------------------------------------------------------------------------
	override void EOnInit(IEntity owner)
	{
		super.EOnInit(owner);

		// Set normal spawn parameters
		m_iRespawnPeriod = NORMAL_RESPAWN_TIME;
		m_iNum = NORMAL_GROUP_COUNT;

		if (DEBUG_MODE)
		{
			PrintFormat("[IPC Extended] Defender spawn point initialized - DEBUG MODE ENABLED (Wave intervals: 1min, 2min, 3min, 4min)", LogLevel.NORMAL);
		}
		else
		{
			Print("[IPC Extended] Defender spawn point with reinforcement capability initialized", LogLevel.NORMAL);
		}

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
	//! Get wave threshold based on debug mode
	//------------------------------------------------------------------------------------------------
	protected int GetWaveThreshold(int waveNumber)
	{
		if (DEBUG_MODE)
		{
			// In debug mode: 1 minute intervals (1min, 2min, 3min, 4min)
			return waveNumber * DEBUG_WAVE_INTERVAL;
		}

		// Normal mode: 5, 10, 15, 20 minutes
		switch (waveNumber)
		{
			case 1: return REINFORCEMENT_WAVE1_THRESHOLD;
			case 2: return REINFORCEMENT_WAVE2_THRESHOLD;
			case 3: return REINFORCEMENT_WAVE3_THRESHOLD;
			case 4: return REINFORCEMENT_WAVE4_THRESHOLD;
		}

		return 999999; // Invalid wave
	}

	//------------------------------------------------------------------------------------------------
	//! Despawn all reinforcement groups and helicopters from previous waves
	//------------------------------------------------------------------------------------------------
	protected void DespawnPreviousWaveGroups()
	{
		if (DEBUG_MODE)
		{
			PrintFormat("[IPC Reinforcement DEBUG] Despawning previous wave groups (%1 groups, %2 helicopters)",
						m_aReinforcementGroups.Count(), m_aReinforcementHelicopters.Count());
		}

		// Despawn all reinforcement groups
		foreach (SCR_AIGroup group : m_aReinforcementGroups)
		{
			if (group && !group.IsDeleted())
			{
				array<AIAgent> units = {};
				group.GetAgents(units);
				RplComponent.DeleteRplEntity(group, false);
			}
		}
		m_aReinforcementGroups.Clear();

		// Despawn all helicopters
		foreach (IEntity helicopter : m_aReinforcementHelicopters)
		{
			if (helicopter && !helicopter.IsDeleted())
			{
				RplComponent.DeleteRplEntity(helicopter, false);
			}
		}
		m_aReinforcementHelicopters.Clear();

		if (DEBUG_MODE)
		{
			PrintFormat("[IPC Reinforcement DEBUG] Previous wave cleanup complete");
		}
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

			if (DEBUG_MODE)
				PrintFormat("[IPC Reinforcement DEBUG] Combat detected at %1 - DEBUG MODE ACTIVE (1 min intervals)",
							m_nearBase.GetOwner().GetName());
			else
				PrintFormat("[IPC Reinforcement] Combat detected at %1 - tracking started",
							m_nearBase.GetOwner().GetName());
		}

		// Reset if combat stopped
		if (!combatActive && m_bReinforcementActive)
		{
			ResetReinforcementState();

			if (DEBUG_MODE)
				PrintFormat("[IPC Reinforcement DEBUG] Combat ended at %1 - reset", m_nearBase.GetOwner().GetName());
			else
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

			// Check waves in priority order (Wave 3 → Wave 2 → Wave 1)
			// Use GetWaveThreshold() to get correct timing based on debug mode
			// NOTE: Wave 4 (helicopter) is temporarily disabled for testing

			// Wave 4 - DISABLED FOR TESTING
			//if (m_iReinforcementWave < 4 && combatDuration >= GetWaveThreshold(4))
			//{
			//	TriggerReinforcements(4);
			//}
			// Wave 3
			if (m_iReinforcementWave < 3 && combatDuration >= GetWaveThreshold(3))
			{
				TriggerReinforcements(3);
			}
			// Wave 2
			else if (m_iReinforcementWave < 2 && combatDuration >= GetWaveThreshold(2))
			{
				TriggerReinforcements(2);
			}
			// Wave 1
			else if (m_iReinforcementWave < 1 && combatDuration >= GetWaveThreshold(1))
			{
				TriggerReinforcements(1);
			}

			// Debug mode: Display time until next wave
			if (DEBUG_MODE)
			{
				int nextWave = m_iReinforcementWave + 1;
				if (nextWave <= 3) // Only up to wave 3 (wave 4 disabled)
				{
					float timeUntilNextWave = GetWaveThreshold(nextWave) - combatDuration;
					if (timeUntilNextWave > 0)
					{
						PrintFormat("[IPC Reinforcement DEBUG] Current wave: %1 | Time until Wave %2: %3 seconds | Combat duration: %4s",
									m_iReinforcementWave, nextWave, timeUntilNextWave, combatDuration);
					}
				}
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

		// Debug mode: Despawn previous wave before spawning new one
		if (DEBUG_MODE)
		{
			DespawnPreviousWaveGroups();
		}

		m_tLastReinforcementTime = world.GetServerTimestamp();
		m_iReinforcementWave = wave;

		string baseName = m_nearBase.GetOwner().GetName();

		// Handle Wave 4 - Armed helicopter with crew
		if (wave == 4)
		{
			if (DEBUG_MODE)
				PrintFormat("[IPC Reinforcement DEBUG] WAVE 4 (4min) triggering at %1 - spawning helicopter", baseName);
			else
				PrintFormat("[IPC Reinforcement] WAVE 4 (20min) triggering at %1 - spawning helicopter", baseName);

			int successfulSpawns = 0;

			// Spawn armed helicopter (Mi-8MT) - TESTING: Just helicopter, no ground team
			IEntity helicopter = SpawnArmedHelicopter();
			if (helicopter)
			{
				PrintFormat("[IPC Reinforcement] Helicopter spawned, checking for compartment manager...");

				// Try to get the vehicle's compartment manager
				SCR_BaseCompartmentManagerComponent compartmentMgr = SCR_BaseCompartmentManagerComponent.Cast(
					helicopter.FindComponent(SCR_BaseCompartmentManagerComponent));

				if (compartmentMgr)
				{
					PrintFormat("[IPC Reinforcement] Found compartment manager, attempting to spawn default occupants...");

					// Try to spawn default crew defined in the vehicle prefab
					if (compartmentMgr.SpawnDefaultOccupants(ECompartmentType.PILOT | ECompartmentType.TURRET))
					{
						successfulSpawns++;
						PrintFormat("[IPC Reinforcement] Wave 4 helicopter spawned with default crew");
					}
					else
					{
						PrintFormat("[IPC Reinforcement] WARNING: SpawnDefaultOccupants returned false");
						successfulSpawns++; // Still count helicopter
					}
				}
				else
				{
					PrintFormat("[IPC Reinforcement] WARNING: Helicopter has no compartment manager");
					successfulSpawns++; // Still count helicopter
				}
			}
			else
			{
				PrintFormat("[IPC Reinforcement] ERROR: Failed to spawn helicopter");
			}

			if (successfulSpawns > 0)
			{
				PrintFormat("[IPC Reinforcement] Successfully spawned Wave 4 reinforcements (%1 elements) at %2",
							successfulSpawns, baseName);
				BroadcastReinforcementAlert(baseName, wave);
			}
			else
			{
				PrintFormat("[IPC Reinforcement] ERROR: Failed to spawn any Wave 4 reinforcements at %1", baseName);
			}

			return;
		}

		// Handle Wave 3 - Combined force (SQUAD_RIFLE + FIRETEAM)
		if (wave == 3)
		{
			if (DEBUG_MODE)
				PrintFormat("[IPC Reinforcement DEBUG] WAVE 3 (3min) triggering at %1 - spawning combined force (SQUAD_RIFLE + FIRETEAM)", baseName);
			else
				PrintFormat("[IPC Reinforcement] WAVE 3 (15min) triggering at %1 - spawning combined force (SQUAD_RIFLE + FIRETEAM)", baseName);

			int successfulSpawns = 0;

			// Spawn SQUAD_RIFLE
			SCR_AIGroup squadGroup = SpawnReinforcementGroup(SCR_EGroupType.SQUAD_RIFLE);
			if (squadGroup)
			{
				m_aReinforcementGroups.Insert(squadGroup);
				successfulSpawns++;
			}

			// Spawn FIRETEAM
			SCR_AIGroup fireteamGroup = SpawnReinforcementGroup(SCR_EGroupType.FIRETEAM);
			if (fireteamGroup)
			{
				m_aReinforcementGroups.Insert(fireteamGroup);
				successfulSpawns++;
			}

			if (successfulSpawns > 0)
			{
				PrintFormat("[IPC Reinforcement] Successfully spawned %1/2 combined force groups at %2", successfulSpawns, baseName);
				BroadcastReinforcementAlert(baseName, wave);
			}
			else
			{
				PrintFormat("[IPC Reinforcement] ERROR: Failed to spawn any Wave 3 groups at %1", baseName);
			}

			return;
		}

		// Handle Waves 1 and 2 - Single group type
		SCR_EGroupType groupType;
		if (wave == 1)
			groupType = SCR_EGroupType.FIRETEAM;
		else
			groupType = SCR_EGroupType.SQUAD_RIFLE;

		string waveTime;
		if (DEBUG_MODE)
		{
			if (wave == 1)
				waveTime = "1min";
			else
				waveTime = "2min";
			PrintFormat("[IPC Reinforcement DEBUG] WAVE %1 (%2) triggering at %3 - spawning %4 reinforcement groups (type: %5)",
						wave, waveTime, baseName, REINFORCEMENT_GROUP_COUNT, typename.EnumToString(SCR_EGroupType, groupType));
		}
		else
		{
			if (wave == 1)
				waveTime = "5min";
			else
				waveTime = "10min";
			PrintFormat("[IPC Reinforcement] WAVE %1 (%2) triggering at %3 - spawning %4 reinforcement groups (type: %5)",
						wave, waveTime, baseName, REINFORCEMENT_GROUP_COUNT, typename.EnumToString(SCR_EGroupType, groupType));
		}

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
	//! Find a position to spawn helicopter at distance (uses terrain-aware positioning)
	//------------------------------------------------------------------------------------------------
	protected vector FindHelicopterSpawnPosition(vector basePos, float distance, float altitude)
	{
		// Calculate random position at distance from base
		float randomAngle = Math.RandomFloat(0, 360);
		float angleRad = randomAngle * Math.DEG2RAD;

		vector targetPos;
		targetPos[0] = basePos[0] + (Math.Cos(angleRad) * distance);
		targetPos[2] = basePos[2] + (Math.Sin(angleRad) * distance);
		targetPos[1] = GetGame().GetWorld().GetSurfaceY(targetPos[0], targetPos[2]);

		// Find empty terrain position for safe spawning (like parent mod does for vehicles)
		vector spawnPos;
		if (!SCR_WorldTools.FindEmptyTerrainPosition(spawnPos, targetPos, 100, 5, 2))
		{
			// Fallback to target position if no empty position found
			spawnPos = targetPos;
		}

		// Add altitude offset (0 for ground spawn)
		spawnPos[1] = spawnPos[1] + altitude;

		return spawnPos;
	}

	//------------------------------------------------------------------------------------------------
	//! Spawn armed helicopter (Mi-8MT) at distance from base
	//------------------------------------------------------------------------------------------------
	protected IEntity SpawnArmedHelicopter()
	{
		if (!m_nearBase)
		{
			Print("[IPC Reinforcement] ERROR: No base reference for helicopter spawn", LogLevel.ERROR);
			return null;
		}

		// Load helicopter prefab
		Resource prefab = Resource.Load(HELICOPTER_PREFAB_MI8MT);
		if (!prefab || !prefab.IsValid())
		{
			PrintFormat("[IPC Reinforcement] ERROR: Failed to load helicopter prefab: %1", HELICOPTER_PREFAB_MI8MT);
			return null;
		}

		// Find spawn position far from base (spawned at altitude, already flying)
		vector basePos = m_nearBase.GetOwner().GetOrigin();
		vector spawnPos = FindHelicopterSpawnPosition(basePos, HELICOPTER_SPAWN_DISTANCE, HELICOPTER_SPAWN_ALTITUDE);

		// Setup spawn parameters
		EntitySpawnParams params = EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = spawnPos;

		// Calculate direction towards base for initial orientation
		vector directionToBase = vector.Direction(spawnPos, basePos);
		directionToBase.Normalize();
		Math3D.MatrixFromForwardVec(directionToBase, params.Transform);
		params.Transform[3] = spawnPos; // Restore position after matrix calculation

		// Spawn the helicopter
		IEntity helicopter = GetGame().SpawnEntityPrefab(prefab, GetGame().GetWorld(), params);
		if (!helicopter)
		{
			Print("[IPC Reinforcement] ERROR: Failed to spawn helicopter entity", LogLevel.ERROR);
			return null;
		}

		PrintFormat("[IPC Reinforcement] Spawned helicopter at position %1 (distance: %2m from base, altitude: %3m)",
					spawnPos, vector.Distance(spawnPos, basePos), HELICOPTER_SPAWN_ALTITUDE);

		// Track helicopter for cleanup
		m_aReinforcementHelicopters.Insert(helicopter);

		return helicopter;
	}

	//------------------------------------------------------------------------------------------------
	//! Spawn helicopter crew as a group near helicopter position
	//------------------------------------------------------------------------------------------------
	protected SCR_AIGroup SpawnHelicopterCrewAtPosition(vector helicopterPos)
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

		// Load group prefab
		Resource prefab = Resource.Load(m_sPrefab);
		if (!prefab || !prefab.IsValid())
		{
			PrintFormat("[IPC Reinforcement] ERROR: Failed to load crew group prefab: %1", m_sPrefab);
			return null;
		}

		// Spawn crew on ground (they'll be moved into helicopter compartments directly)
		vector crewSpawnPos = helicopterPos;
		crewSpawnPos[1] = GetGame().GetWorld().GetSurfaceY(helicopterPos[0], helicopterPos[2]); // Ground level

		// Find empty position on ground
		vector emptyPos;
		if (SCR_WorldTools.FindEmptyTerrainPosition(emptyPos, crewSpawnPos, 50, 5, 2))
		{
			crewSpawnPos = emptyPos;
		}

		// Setup spawn parameters
		EntitySpawnParams params = EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = crewSpawnPos;

		// Spawn the group entity
		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(prefab, null, params));
		if (!group)
		{
			Print("[IPC Reinforcement] ERROR: Failed to spawn crew group entity", LogLevel.ERROR);
			return null;
		}

		// Spawn units within the group (need at least 4 crew for Mi-8: pilot, copilot, 2 gunners)
		if (!group.GetSpawnImmediately())
		{
			// Call SpawnUnits() 4 times to spawn a full helicopter crew
			for (int i = 0; i < 4; i++)
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

			// Set AI skill based on player count (same as infantry)
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

		PrintFormat("[IPC Reinforcement] Spawned helicopter crew with %1 agents (will be moved into compartments)", agents.Count());

		return group;
	}

	//------------------------------------------------------------------------------------------------
	//! Move crew directly into helicopter compartments using MoveInVehicle
	//------------------------------------------------------------------------------------------------
	protected bool CrewHelicopter(IEntity helicopter, SCR_AIGroup crewGroup)
	{
		if (!helicopter || !crewGroup)
			return false;

		// Get all agents from the crew group
		array<AIAgent> agents = {};
		crewGroup.GetAgents(agents);

		if (agents.Count() == 0)
		{
			Print("[IPC Reinforcement] ERROR: No agents in crew group to move into helicopter", LogLevel.ERROR);
			return false;
		}

		int successfulBoards = 0;

		// Move each crew member into helicopter compartments
		foreach (int i, AIAgent agent : agents)
		{
			IEntity agentEntity = agent.GetControlledEntity();
			if (!agentEntity)
				continue;

			// Get compartment access component
			SCR_CompartmentAccessComponent compartmentAccess = SCR_CompartmentAccessComponent.Cast(
				agentEntity.FindComponent(SCR_CompartmentAccessComponent));

			if (!compartmentAccess)
			{
				PrintFormat("[IPC Reinforcement] WARNING: Agent %1 has no SCR_CompartmentAccessComponent", i);
				continue;
			}

			// Assign compartment types: first agent = pilot, rest = turret/cargo
			ECompartmentType compartmentType;
			if (i == 0)
				compartmentType = ECompartmentType.PILOT;
			else
				compartmentType = ECompartmentType.TURRET; // Gunner positions

			// Move agent into vehicle compartment
			if (compartmentAccess.MoveInVehicle(helicopter, compartmentType))
			{
				successfulBoards++;
				PrintFormat("[IPC Reinforcement] Crew member %1 entered helicopter as %2",
					i, typename.EnumToString(ECompartmentType, compartmentType));
			}
			else
			{
				PrintFormat("[IPC Reinforcement] WARNING: Failed to move crew member %1 into helicopter", i);
			}
		}

		PrintFormat("[IPC Reinforcement] Successfully moved %1/%2 crew members into helicopter",
					successfulBoards, agents.Count());

		return successfulBoards > 0;
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

		// In debug mode, immediately despawn all reinforcements when combat ends
		if (DEBUG_MODE)
		{
			DespawnPreviousWaveGroups();
		}

		// Normal mode: Cleanup is handled by CleanupDeadReinforcementGroups() periodic check
		// Groups will despawn naturally when all agents die
	}

	//------------------------------------------------------------------------------------------------
	//! Check if this base should keep defenders active (frontline detection)
	//------------------------------------------------------------------------------------------------
	protected bool ShouldKeepDefendersActive()
	{
		if (!m_nearBase)
			return false;

		// RULE 1: Only apply to friendly bases
		if (!IsBaseFriendly(m_nearBase))
			return true; // Keep enemy defenders always active

		// RULE 2: Check if base is on frontline (enemy base nearby)
		if (IsBaseOnFrontline(m_nearBase))
		{
			m_tInactiveSince = null; // Reset grace period
			return true; // Frontline base - keep active
		}

		// Base is not on frontline - apply grace period before despawning
		// The 10-minute grace period handles temporary combat situations
		ChimeraWorld world = GetOwner().GetWorld();
		if (!world)
			return true;

		WorldTimestamp currentTime = world.GetServerTimestamp();

		// Start grace period if not already started
		if (!m_tInactiveSince)
		{
			m_tInactiveSince = currentTime;
			PrintFormat("[IPC Defender] Base %1 became inactive - grace period started (10min)",
						m_nearBase.GetOwner().GetName());
			return true; // Keep active during grace period
		}

		// Check if grace period expired
		float inactiveDuration = currentTime.DiffMilliseconds(m_tInactiveSince) / 1000.0;
		if (inactiveDuration >= INACTIVE_GRACE_PERIOD)
		{
			PrintFormat("[IPC Defender] Base %1 inactive for %2s - despawning defenders",
						m_nearBase.GetOwner().GetName(), inactiveDuration);
			return false; // Grace period expired - despawn
		}

		// Still in grace period
		if (DEBUG_MODE)
		{
			float timeRemaining = INACTIVE_GRACE_PERIOD - inactiveDuration;
			PrintFormat("[IPC Defender DEBUG] Base %1 inactive - %2s until despawn",
						m_nearBase.GetOwner().GetName(), timeRemaining);
		}

		return true; // Keep active during grace period
	}

	//------------------------------------------------------------------------------------------------
	//! Check if base is controlled by a friendly faction (player's faction)
	//------------------------------------------------------------------------------------------------
	protected bool IsBaseFriendly(SCR_CampaignMilitaryBaseComponent base)
	{
		Faction baseFaction = base.GetFaction();
		if (!baseFaction)
			return false;

		// Check if any player is on this base's faction
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

			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(player);
			if (!character)
				continue;

			Faction playerFaction = character.GetFaction();
			if (playerFaction == baseFaction)
				return true; // Base is friendly to at least one player
		}

		return false; // No players on this faction
	}

	//------------------------------------------------------------------------------------------------
	//! Check if base is on frontline (enemy base within attack range)
	//------------------------------------------------------------------------------------------------
	protected bool IsBaseOnFrontline(SCR_CampaignMilitaryBaseComponent base)
	{
		SCR_GameModeCampaign gameMode = SCR_GameModeCampaign.GetInstance();
		if (!gameMode)
			return true;

		SCR_CampaignMilitaryBaseManager baseManager = gameMode.GetBaseManager();
		if (!baseManager)
			return true;

		SCR_CampaignFactionManager factionManager = SCR_CampaignFactionManager.Cast(GetGame().GetFactionManager());
		if (!factionManager)
			return true;

		// Get enemy faction
		SCR_CampaignFaction ourFaction = SCR_CampaignFaction.Cast(m_Faction);
		if (!ourFaction)
			return true;

		SCR_CampaignFaction enemyFaction = factionManager.GetEnemyFaction(ourFaction);
		if (!enemyFaction)
			return true;

		// Check if any enemy base is within attack range
		array<SCR_CampaignMilitaryBaseComponent> enemyBases = {};
		baseManager.GetBases(enemyBases, enemyFaction);

		vector ourPos = base.GetOwner().GetOrigin();
		foreach (SCR_CampaignMilitaryBaseComponent enemyBase : enemyBases)
		{
			float distance = vector.Distance(ourPos, enemyBase.GetOwner().GetOrigin());
			if (distance < FRONTLINE_RANGE)
				return true; // Enemy base nearby - this is frontline
		}

		return false; // No enemy bases nearby - this is rear area
	}

	//------------------------------------------------------------------------------------------------
	//! Override UpdateTarget to use frontline detection logic (friendly bases only)
	//------------------------------------------------------------------------------------------------
	override void UpdateTarget()
	{
		PrepareBase();
		if (!m_bBaseReady)
			return;

		if (!m_nearBase)
		{
			SetIsNearTarget(false);
			return;
		}

		// Only apply frontline detection to friendly bases
		// Enemy bases use the parent mod's default behavior (always active when base exists)
		if (!IsBaseFriendly(m_nearBase))
		{
			// Use parent mod's default behavior for enemy bases
			SetIsNearTarget(true);
			SetIsTargetChanged(false);
			return;
		}

		// For friendly bases, use frontline detection logic
		bool shouldKeepActive = ShouldKeepDefendersActive();
		SetIsNearTarget(shouldKeepActive);

		if (!shouldKeepActive && DEBUG_MODE)
		{
			PrintFormat("[IPC Defender DEBUG] Base %1 marked for despawn (not on frontline)",
						m_nearBase.GetOwner().GetName());
		}

		SetIsTargetChanged(false);
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
