//------------------------------------------------------------------------------------------------
// IPC AI Combat Extended - Modded Base Spawn Point Class
// Extends base IPC mod to adjust AI perception for solo players
// Requirements: Solo players (1 player) get 1.0x perception instead of 1.5x
//------------------------------------------------------------------------------------------------

modded class IPC_SpawnPointComponent : ScriptComponent
{
	//------------------------------------------------------------------------------------------------
	//! Override SpawnPatrol to adjust AI perception for solo players
	//! Keeps EXPERT skill level but reduces perception to 1.0x for single player
	override void SpawnPatrol()
	{
		// Call parent implementation to handle all spawn logic
		super.SpawnPatrol();

		// Get current player count
		int players = GetGame().GetPlayerManager().GetPlayerCount();

		// Only adjust for solo players
		if (players == 1)
		{
			// Get all agents in the spawned group
			array<AIAgent> agents = {};
			if (m_Group)
			{
				m_Group.GetAgents(agents);

				// Adjust perception for each AI agent
				foreach (AIAgent agent : agents)
				{
					IEntity agentEntity = agent.GetControlledEntity();
					if (!agentEntity)
						continue;

					SCR_AIInfoComponent m_InfoComponent = SCR_AIInfoComponent.Cast(agentEntity.FindComponent(SCR_AIInfoComponent));
					if (!m_InfoComponent)
						continue;

					SCR_AICombatComponent CombatComponent = m_InfoComponent.GetCombatComponent();
					if (!CombatComponent)
						continue;

					// Keep EXPERT skill, but reduce perception to 1.0x for solo players
					CombatComponent.SetAISkill(EAISkill.EXPERT);
					CombatComponent.SetPerceptionFactor(1.0);

					// Debug logging
					PrintFormat("[IPC Extended] Solo player mode - AI skill: EXPERT, Perception: 1.0x");
				}
			}
		}
	}
}
