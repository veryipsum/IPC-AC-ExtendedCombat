//------------------------------------------------------------------------------------------------
// IPC AI Combat Extended - Modded Attacker Spawn Component
// Extends base IPC mod to modify attacking friendly spawn behavior
// Requirements: 100s respawn time, 3 groups per spawn point
//------------------------------------------------------------------------------------------------

modded class IPC_AutonomousCaptureSpawnPointComponent : IPC_SpawnPointComponent
{
	//------------------------------------------------------------------------------------------------
	//! Override initialization to set custom spawn parameters for attacking friendlies
	//------------------------------------------------------------------------------------------------
	override void EOnInit(IEntity owner)
	{
		super.EOnInit(owner);

		// Set spawn parameters for attacking friendly forces
		// User requirement: 100s respawn (was 120s), 3 groups (was 2)
		m_iRespawnPeriod = 100;  // Respawn time in seconds
		m_iNum = 3;              // Number of groups to spawn

		PrintFormat("[IPC Extended] Attacker spawn point initialized - Respawn: %1s, Groups: %2",
					m_iRespawnPeriod, m_iNum);
	}
}
