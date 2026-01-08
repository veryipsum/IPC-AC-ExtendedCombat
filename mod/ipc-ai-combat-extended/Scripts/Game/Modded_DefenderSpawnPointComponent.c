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
	protected vector m_vOriginalSpawnPosition;				// Store original spawn point position for restoration

	// Reinforcement configuration
	protected const int REINFORCEMENT_WAVE1_THRESHOLD = 300;	// 5 minutes
	protected const int REINFORCEMENT_WAVE2_THRESHOLD = 600;	// 10 minutes
	protected const float COMBAT_DETECTION_RANGE = 300.0;		// Distance to detect player activity

	// Normal spawn parameters
	protected const int NORMAL_RESPAWN_TIME = 180;
	protected const int NORMAL_GROUP_COUNT = 2;

	// Reinforcement spawn parameters
	protected const int REINFORCEMENT_GROUP_COUNT = 3;			// Spawn more groups
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

		// Start periodic reinforcement check (every 5 seconds)
		GetGame().GetCallqueue().CallLater(CheckReinforcements, 5000, true);
	}

	//------------------------------------------------------------------------------------------------
	//! Periodic check for reinforcements (called every 5 seconds)
	//------------------------------------------------------------------------------------------------
	protected void CheckReinforcements()
	{
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
	//! Trigger reinforcement wave
	//------------------------------------------------------------------------------------------------
	protected void TriggerReinforcements(int wave)
	{
		// Record the wave time to prevent double-triggering
		ChimeraWorld world = GetOwner().GetWorld();
		if (!world)
			return;

		m_tLastReinforcementTime = world.GetServerTimestamp();
		m_iReinforcementWave = wave;

		string baseName = m_nearBase.GetOwner().GetName();

		// Don't despawn existing defenders - let them keep fighting
		// Reinforcements will spawn in addition to current defenders

		// Save original spawn point position
		IEntity spawnPointEntity = GetOwner();
		m_vOriginalSpawnPosition = spawnPointEntity.GetOrigin();

		// Calculate offset position for reinforcement spawn (100m away from base center)
		vector basePos = m_nearBase.GetOwner().GetOrigin();
		vector spawnPos = m_vOriginalSpawnPosition;

		// Get direction from base to spawn point, extend it outward
		vector directionFromBase = vector.Direction(basePos, spawnPos);
		if (directionFromBase.Length() < 1.0)
		{
			// If spawn point is at base center, pick random direction
			float randomAngle = Math.RandomFloat(0, 360);
			directionFromBase[0] = Math.Cos(randomAngle * Math.DEG2RAD);
			directionFromBase[2] = Math.Sin(randomAngle * Math.DEG2RAD);
		}
		directionFromBase.Normalize();

		// Offset spawn position 100m from base in that direction
		vector reinforcementSpawnPos = basePos + (directionFromBase * 100.0);

		// Set temporary spawn position
		spawnPointEntity.SetOrigin(reinforcementSpawnPos);
		PrintFormat("[IPC Reinforcement] Moved spawn point to perimeter (~100m from base center)");

		// Temporarily boost spawn parameters
		m_iNum = REINFORCEMENT_GROUP_COUNT;
		m_fSpawnDisperson = REINFORCEMENT_SPAWN_RADIUS;

		if (wave == 1)
		{
			m_eGroupType = SCR_EGroupType.FIRETEAM;
			PrintFormat("[IPC Reinforcement] WAVE 1 at %1 - spawning %2 FIRETEAM groups",
						baseName, REINFORCEMENT_GROUP_COUNT);
		}
		else
		{
			m_eGroupType = SCR_EGroupType.SQUAD_RIFLE;
			PrintFormat("[IPC Reinforcement] WAVE 2 at %1 - spawning %2 SQUAD groups",
						baseName, REINFORCEMENT_GROUP_COUNT);
		}

		// Broadcast notification to all players
		BroadcastReinforcementAlert(baseName, wave);

		// Force immediate spawn by expiring the respawn timer and calling SpawnPatrol
		m_fRespawnTimestamp = world.GetServerTimestamp(); // Make timer already expired
		m_bSpawned = false; // Mark as not spawned
		m_iMembersAlive = 0; // Mark group as dead

		// Directly call SpawnPatrol to force immediate spawn (will use temporary position)
		PrintFormat("[IPC Reinforcement] Forcing immediate spawn at %1", baseName);
		SpawnPatrol();

		// Restore original spawn point position immediately after spawn
		spawnPointEntity.SetOrigin(m_vOriginalSpawnPosition);
		PrintFormat("[IPC Reinforcement] Restored spawn point to original position");

		// Schedule reset to normal parameters after spawn
		GetGame().GetCallqueue().CallLater(ResetToNormalSpawn, 10000, false);
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
	//! Reset to normal spawn parameters
	//------------------------------------------------------------------------------------------------
	protected void ResetToNormalSpawn()
	{
		m_iNum = NORMAL_GROUP_COUNT;
		m_fSpawnDisperson = 50.0; // Reset to normal
		Print("[IPC Reinforcement] Returned to normal spawn parameters", LogLevel.VERBOSE);
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
}
