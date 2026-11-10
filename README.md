# SifuTAS

## V1.0 - A speed control tool for Sifu
### Allowing you to pause, continue frame by frame, speed up or slow down the game.

## [Download V1.0](https://github.com/EgeEken/SifuTAS/releases/download/v1.0/dsound.dll)

Install by placing the dsound.dll file directly into the `...\steamapps\common\Sifu\Sifu\Binaries\Win64\` path.

## Usage

### Press F1 to open the GUI overlay


### Pause, resume and continue the game frame by frame


### Slow down the game


### Set and use keybinds for easier control


### Speed up the game for a harder challenge if sifu feels too easy for you?



## Roadmap?

Initially planned to include savestating and replay mechanics for a proper TAS tool but it didn't work that well, seems like Sifu is not really the easiest game to build that for, probably why such a tool didn't exist in the first place.
For now, the project is stopped, but i might return later at some point to turn this into a full TAS tool. If i do, the main features i want to implement are:

- Savestating, save and load to a given game state
- Deterministic full speed replay, record your gameplay in terms of game states, accounting for slow downs, loaded savestates, etc, and replay it at 1.0x speed
