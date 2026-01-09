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
	protected bool m_bReinforcementSpawnPending = false;	// Is a reinforcement spawn waiting for natural cycle?

	// Reinforcement configuration
	protected const int REINFORCEMENT_WAVE1_THRESHOLD = 300;	// 5 minutes
	protected const int REINFORCEMENT_WAVE2_THRESHOLD = 600;	// 10 minutes
	protected const float COMBAT_DETECTION_RANGE = 300.0;		// Distance to detect player activity
	protected const int CHECK_INTERVAL = 30000;					// Check every 30 seconds (not 5!)

	// Normal spawn parameters
	protected const int NORMAL_RESPAWN_TIME = 180;
	protected const int NORMAL_GROUP_COUNT = 2;

	// Reinforcement spawn parameters
	protected const int REINFORCEMENT_GROUP_COUNT = 2;			// Spawn more groups
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
	//! Trigger reinforcement wave - sets up params and waits for parent mod's natural spawn cycle
	//------------------------------------------------------------------------------------------------
	protected void TriggerReinforcements(int wave)
	{
		ChimeraWorld world = GetOwner().GetWorld();
		if (!world)
		{
			Print("[IPC Reinforcement] ERROR: No world in TriggerReinforcements", LogLevel.ERROR);
			return;
		}

		// Prevent triggering if already spawned
		if (m_bSpawned)
		{
			PrintFormat("[IPC Reinforcement] WARNING: Reinforcement trigger blocked - group already spawned at %1",
						m_nearBase.GetOwner().GetName());
			return;
		}

		m_tLastReinforcementTime = world.GetServerTimestamp();
		m_iReinforcementWave = wave;
		m_bReinforcementSpawnPending = true;

		string baseName = m_nearBase.GetOwner().GetName();

		// Set reinforcement parameters BEFORE despawning
		// This prevents race conditions with parent mod's spawn system
		m_iNum = REINFORCEMENT_GROUP_COUNT;
		m_fSpawnDisperson = REINFORCEMENT_SPAWN_RADIUS;

		if (wave == 1)
		{
			m_eGroupType = SCR_EGroupType.FIRETEAM;
			PrintFormat("[IPC Reinforcement] WAVE 1 queued at %1 - will spawn %2 FIRETEAM groups on next cycle",
						baseName, REINFORCEMENT_GROUP_COUNT);
		}
		else
		{
			m_eGroupType = SCR_EGroupType.SQUAD_RIFLE;
			PrintFormat("[IPC Reinforcement] WAVE 2 queued at %1 - will spawn %2 SQUAD groups on next cycle",
						baseName, REINFORCEMENT_GROUP_COUNT);
		}

		// Broadcast notification
		BroadcastReinforcementAlert(baseName, wave);

		// If there's an existing group, despawn it to make room for reinforcements
		if (m_Group)
		{
			PrintFormat("[IPC Reinforcement] Despawning current group to make room for reinforcements at %1", baseName);
			DespawnPatrol();
		}

		// Let parent mod's natural spawn cycle handle the actual spawning
		// Just expire the respawn timer so it spawns on next ProcessSpawnpoint() call
		m_fRespawnTimestamp = world.GetServerTimestamp();

		PrintFormat("[IPC Reinforcement] Reinforcement spawn queued at %1 - waiting for parent spawn system", baseName);

		// Schedule reset to normal parameters after spawn completes
		GetGame().GetCallqueue().CallLater(CheckAndResetReinforcementParams, 15000, false);
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
		IPC_AutonomousCaptureSystem autonomousCaptureSystem = IPC_AutonomousCaptureSystem.GetInstance();
		if (!autonomousCaptureSystem)
		{
			Print("[IPC Reinforcement] Failed to get AutonomousCaptureSystem for notification");
			return;
		}

		PlayerManager playerManager = GetGame().GetPlayerManager();
		if (!playerManager)
			return;

		string title = "Enemy Reinforcements Detected";
		string message = string.Format("AO: %1", baseName);

		array<int> playerIds = {};
		playerManager.GetPlayers(playerIds);

		// Broadcast to all players
		foreach (int playerId : playerIds)
		{
			IEntity player = playerManager.GetPlayerControlledEntity(playerId);
			if (!player)
				continue;

			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(player);
			if (!character)
				continue;

			// Send notification to player with their faction key
			autonomousCaptureSystem.PopUpMessage(title, message, character.GetFactionKey(), playerId);
		}

		PrintFormat("[IPC Reinforcement] Sent notification to %1 players about reinforcements at %2", playerIds.Count(), baseName);
	}

	//------------------------------------------------------------------------------------------------
	//! Check if reinforcement spawn completed, then reset params
	//------------------------------------------------------------------------------------------------
	protected void CheckAndResetReinforcementParams()
	{
		if (!m_bReinforcementSpawnPending)
		{
			Print("[IPC Reinforcement] No pending reinforcement reset needed", LogLevel.VERBOSE);
			return;
		}

		if (m_bSpawned && m_Group)
		{
			PrintFormat("[IPC Reinforcement] Reinforcement spawn confirmed at %1 - resetting to normal params",
						m_nearBase.GetOwner().GetName());
			m_bReinforcementSpawnPending = false;
			ResetToNormalSpawn();
		}
		else
		{
			PrintFormat("[IPC Reinforcement] WARNING: Reinforcement spawn did not complete at %1 - retrying reset in 5s",
						m_nearBase.GetOwner().GetName());
			// Try again in 5 seconds
			GetGame().GetCallqueue().CallLater(CheckAndResetReinforcementParams, 5000, false);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Reset to normal spawn parameters
	//------------------------------------------------------------------------------------------------
	protected void ResetToNormalSpawn()
	{
		m_iNum = NORMAL_GROUP_COUNT;
		m_fSpawnDisperson = 50.0; // Reset to normal
		PrintFormat("[IPC Reinforcement] Reset to normal spawn parameters (Groups: %1, Dispersion: 50m)", NORMAL_GROUP_COUNT);
	}

	//------------------------------------------------------------------------------------------------
	//! Reset reinforcement tracking
	//------------------------------------------------------------------------------------------------
	protected void ResetReinforcementState()
	{
		m_bReinforcementActive = false;
		m_iReinforcementWave = 0;
		ResetToNormalSpawn();
	}

	//------------------------------------------------------------------------------------------------
	//! Destructor - cleanup scheduled callbacks
	//------------------------------------------------------------------------------------------------
	void ~IPC_DefenderSpawnPointComponent()
	{
		// Remove the periodic check callback if we're the coordinator
		if (m_bIsReinforcementCoordinator)
		{
			GetGame().GetCallqueue().Remove(CheckReinforcements);
			PrintFormat("[IPC Reinforcement] Cleaned up coordinator callbacks for %1", GetOwner().GetName());
		}
	}
}
