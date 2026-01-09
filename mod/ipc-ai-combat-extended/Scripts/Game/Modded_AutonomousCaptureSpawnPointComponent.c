//------------------------------------------------------------------------------------------------
// IPC AI Combat Extended - Modded Attacker Spawn Component
// Extends base IPC mod to modify attacking friendly spawn behavior
// Requirements: 100s respawn time, 1 groups per spawn point
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
		// User requirement: 100s respawn (was 120s), 1 group (was 2)
		m_iRespawnPeriod = 90;   // Respawn time in seconds
		m_iNum = 1;              // Number of SpawnUnits() calls

		PrintFormat("[IPC Extended] Attacker spawn point initialized - Respawn: %1s, Groups: %2",
					m_iRespawnPeriod, m_iNum);
	}
}
