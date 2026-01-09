Note: this is a public testing release; there are currently no known bugs and the mod should be stable; however no extensive testing has yet been done and issues are likely to appear.

The current build of this mod is based on the 'dev' branch of IPC Autonomous Capture (vers: 2.1.10); functionality can fail after updates to the main mod.

I extend my thanks to the IPC Autonomous Capture mod team for their work!

Main features of the Extended Combat addon:
- Adjusted all AI to use Perception value 1.0 if playing single-player
- Adjusted OPFOR to have slightly higher spawn rates (both number of units and frequency of spawning)
- Adjusted BLUFOR to also have slightly higher spawn but comparably lesser than OPFOR
- Implemented a "Reinforcement" system for OPFOR

How does the "Reinforcement" system work?
When any number of players is fighting within 300 meters of a base a timer begins:
- At 5 minutes of fighting Wave 1 of Reinforcements spawns in a random 100-300m range spread from the Base that is under attack (the spawn location is random within that range)
- At 10 minutes of fighting Wave 2 of Reinfocements spawns within the same range and spawn rules as Wave 1.
Wave 1 consists of 1 Group: Fire Team; Wave 2 consists of 1 group: Rifle Squad
These reinforcements are independent from the defenses spawned by the base that's being attacked; meaning that in long extended combat enemy forces can become overwhelming.
- Beyond 10 minutes no more reinforcements will spawn for that base

The timer for reinforcements resets under these conditions:
- All players in range of the base died;
- All players in range of the base left;
- The base has begun being captured (to note: if the base is no longer actively being captured; for ex. if the player capturing dies but there are other players in range of the base; the timer restarts from 0)
- The timer is always reset to 0 when beginning to capture the base

Features being worked on (not yet ready):
- Difficulty selector
- Balance OPFOR spawns to allow for more enemy units to be fielded at the same time if difficulty is set higher
- Additional Waves and maybe include Vehicle spawns (Myself am interested in some kind of Helicopter action)

You can reach me on Discord at: e2631_

Disclaimer: AI tools were used to aid the development of this addon. I respect and support visibility into how AI is used in creative work and pledge myself to provide clarity into how I personally use AI in published work.
1. ChatGPT 5 was used to ideate the scope of the mod; specifically what can be easily achieved for a modding beginner (which I am; this is my 1st ever Arma mod); additionally ChatGPT was used to learn and explore the usage of the Enfusion Workbench.
2. Claude CLI was used to explore, explain and document locally the source code of the original IPC Autonomous Capture mod. Claude CLI was also used to fix bugs when developing the mod as well as write the inline comments for code documentation.
