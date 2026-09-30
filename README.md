# Holiday Control

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that lets you run the server's
holidays and world events from its config file, and add bonus events of your own.

- **Force any event on or off.** Hallow's End all year, no Love is in the Air, the Darkmoon Faire
  always in Elwynn. It stays that way through restarts until you change the config.
- **Reschedule events.** Move a holiday to other dates, make it longer, or repeat it more often
  (Brewfest for two weeks every two months).
- **Bonus events.** Double XP weekends, extra reputation, gold, gathering skill-ups or drop
  chances. Run them on a schedule, while a holiday is on (more reputation during Brewfest), or
  switch them on by hand.
- **Players are told.** A chat message to everyone when a holiday or bonus event starts and ends,
  a note at login about what's running, and `.holidays` to see what's on now and coming up.

Everything is set in `mod_holiday_control.conf`. Edit it, type `.reload config`, and the changes
apply right away without a restart.

## How it works

The core's game event scheduler still starts and stops the events, with their spawns, quests and
vendors. It asks the scripts about each event right before it checks whether the event should run,
and this module answers with the schedule from the config. The event's own schedule is kept, and
it's put back when the config no longer controls the event. The config always wins: a GM's
`.event start` or `.event stop` on a controlled event is undone on the next check.

Only scheduled game events can be controlled. World events that move through their own stages
(Scourge Invasion, the AQ War Effort, Sun's Reach, Wintergrasp) have their own commands in the
core, like `.worldstate scourgeinvasion state 1`.

## Config

Dates are `YYYY-MM-DD` or `YYYY-MM-DD HH:MM` in the worldserver's local time. Durations look like
`7d`, `2d12h`, `90m` or `1w`.

Game events are set by their id (`.holidays list` shows them all):

```ini
# Darkmoon Faire always in Elwynn Forest, never anywhere else
HolidayControl.Event.4.Mode = "on"
HolidayControl.Event.3.Mode = "off"
HolidayControl.Event.5.Mode = "off"

# Brewfest every 2 months for 2 weeks, starting October 1st
HolidayControl.Event.24.Start = "2026-10-01"
HolidayControl.Event.24.Length = "14d"
HolidayControl.Event.24.Repeat = "60d"
```

| Setting | Meaning |
| --- | --- |
| `Event.<id>.Mode` | `auto` (its schedule, the default), `on` (always running), `off` (never runs) |
| `Event.<id>.Start` | First start date |
| `Event.<id>.Length` | How long each run lasts |
| `Event.<id>.Repeat` | Time from one start to the next, `0` = once |
| `Event.<id>.End` | No run starts after this date |
| `Event.<id>.Name` | Name players see |
| `Event.<id>.Show` | `1`/`0`: show it to players. Holidays are shown by default. |

Bonus events get a key of your choice:

```ini
# Double XP every weekend, Friday 18:00 to Monday 06:00
HolidayControl.Bonus.DoubleXP.Name = "Double XP Weekend"
HolidayControl.Bonus.DoubleXP.Start = "2026-10-02 18:00"
HolidayControl.Bonus.DoubleXP.Length = "2d12h"
HolidayControl.Bonus.DoubleXP.Repeat = "7d"
HolidayControl.Bonus.DoubleXP.XP = 2

# +50% reputation while Brewfest runs
HolidayControl.Bonus.BrewfestRep.During = "24"
HolidayControl.Bonus.BrewfestRep.Reputation = 1.5
```

| Setting | Meaning |
| --- | --- |
| `Bonus.<key>.Name` | Name players see (default: the key) |
| `Bonus.<key>.Mode` | `auto` (the default), `on`, `off` |
| `Bonus.<key>.Start` / `Length` / `Repeat` / `End` | Its schedule, like game events |
| `Bonus.<key>.During` | Game event ids; it also runs while any of them runs |
| `Bonus.<key>.XP` | Experience from kills, quests and exploration |
| `Bonus.<key>.Reputation` | Reputation gains (losses are not changed) |
| `Bonus.<key>.Gold` | Gold looted from creatures and chests |
| `Bonus.<key>.Gathering` | Skill points per mining, herbalism and skinning skill-up |
| `Bonus.<key>.Drops` | Chance of loot that rolls on its own: quest items, world drops, rares |

Rates are multipliers on top of the server's own rates. When two bonus events run at once they
don't stack; each rate uses the higher one.

The `.conf.dist` explains every setting and has a list of common event ids.

## Commands

| Command | Who | What it does |
| --- | --- | --- |
| `.holidays` | everyone | Holidays and bonus events running now and coming up in the next 30 days |
| `.holidays list [name]` | GM, console | Every game event with its id, state, next start and whether the config controls it, plus the bonus events |

## Install

Clone it into your AzerothCore `modules` folder **as `mod-holiday-control`**, without the repo's
`wow-` prefix. AzerothCore finds the module's entry point from the folder name.

```bash
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-holiday-control.git mod-holiday-control
```

Rebuild the worldserver, then copy `conf/mod_holiday_control.conf.dist` to your config folder's
`modules` directory as `mod_holiday_control.conf`. There is no SQL.

## Notes

- The client's calendar shows holidays on their standard dates, whatever the server does.
- A holiday's setup stage is its own event (Brewfest's tents, the Darkmoon Faire's construction).
  Moving the main event doesn't move the setup stage.
- Bonus XP also applies to playerbots.

## License

MIT
