/*
 * mod-holiday-control
 *
 * Run the server's holidays and world events from the config file: force any of them on or off,
 * move them to other dates, and add bonus events of your own (double XP, more reputation, gold,
 * gathering skill-ups or drops). Players are told when something starts and ends, see what's on
 * when they log in, and can type .holidays for the list.
 *
 * The core decides whether a game event runs from its start, end, interval and length, and calls
 * the scripts' OnEventCheck right before it checks each event. That's where this module writes the
 * config's schedule into the event, so the core's own scheduler still does the starting and
 * stopping, with the spawns, quests and vendors that come with it. The event's own schedule is
 * remembered and put back when the config stops controlling it. The config always wins over a GM's
 * .event start/stop: the schedule is written again on the next check.
 *
 * Only normal (scheduled) game events can be controlled. World events that advance through their
 * own states (Scourge Invasion, the AQ War Effort, Sun's Reach) don't run on a schedule.
 *
 * Released under the MIT License.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "Log.h"
#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* EVENT_PREFIX = "HolidayControl.Event.";
    constexpr char const* BONUS_PREFIX = "HolidayControl.Bonus.";
    constexpr char const* HOLIDAY_TAG = "|cffff8000[Holiday]|r ";
    constexpr char const* BONUS_TAG = "|cff1eff00[Bonus]|r ";

    constexpr time_t FAR_FUTURE = 4102444800;           // 2100-01-01
    constexpr uint32 FORCED_ON_MINUTES = YEAR / MINUTE; // interval and length of a forced-on event

    enum class Mode : uint8
    {
        Auto,
        On,
        Off,
    };

    // Repeating time windows: the first opens at start, then one every period, each open for length
    // seconds, and none opens at or after end. A one-off has period == length.
    struct Window
    {
        time_t start = 0;
        uint32 length = 0;
        uint32 period = 0;
        time_t end = FAR_FUTURE;

        [[nodiscard]] bool IsValid() const { return length > 0 && period >= length; }

        [[nodiscard]] bool Contains(time_t now) const
        {
            return IsValid() && start <= now && now < end && (now - start) % period < length;
        }

        // When the window that holds now closes. Only meaningful while Contains(now).
        [[nodiscard]] time_t CurrentEnd(time_t now) const
        {
            return std::min<time_t>(now - (now - start) % period + length, end);
        }

        // The next time a window opens after now, or 0 if none will.
        [[nodiscard]] time_t NextStart(time_t now) const
        {
            if (!IsValid() || now >= end)
                return 0;

            if (start > now)
                return start;

            time_t next = start + ((now - start) / period + 1) * period;
            return next < end ? next : 0;
        }
    };

    // A schedule from the config: start is required, the rest falls back to the event's own values
    // (for game events) or to "once, never ending" (for bonus events).
    Window MakeWindow(time_t start, uint32 length, uint32 repeat, time_t end)
    {
        Window window;
        window.start = start;
        window.length = length;
        window.end = end;

        if (repeat)
            window.period = std::max(repeat, length);
        else
        {
            window.period = length;
            window.end = std::min<time_t>(end, start + length);
        }

        return window;
    }

    struct EventRule
    {
        Mode mode = Mode::Auto;
        std::string name;
        std::optional<bool> show;
        std::optional<time_t> start;
        std::optional<time_t> end;
        std::optional<uint32> length; // seconds
        std::optional<uint32> repeat; // seconds, 0 = once

        [[nodiscard]] bool Reschedules() const { return start || end || length || repeat; }
        [[nodiscard]] bool Controls() const { return mode != Mode::Auto || Reschedules(); }
    };

    enum BonusStat : uint8
    {
        BONUS_XP,
        BONUS_REPUTATION,
        BONUS_GOLD,
        BONUS_GATHERING,
        BONUS_DROPS,
        MAX_BONUS_STATS
    };

    constexpr char const* BONUS_KEYS[MAX_BONUS_STATS] = { "xp", "reputation", "gold", "gathering", "drops" };
    constexpr char const* BONUS_NAMES[MAX_BONUS_STATS] =
        { "experience", "reputation", "looted gold", "gathering skill-ups", "drop chances" };

    struct Bonus
    {
        std::string name;
        Mode mode = Mode::Auto;
        std::optional<time_t> start;
        std::optional<time_t> end;
        std::optional<uint32> length;
        std::optional<uint32> repeat;
        Window window;                  // only used when start is set
        std::vector<uint16> during;     // also active while any of these game events runs
        float rates[MAX_BONUS_STATS] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        bool active = false;
    };

    struct Config
    {
        bool enabled = true;
        bool announce = true;
        bool loginMessage = true;
        uint32 upcomingDays = 30;
        std::map<uint16, EventRule> events;
        std::map<std::string, Bonus> bonuses; // by the key used in the config
    };

    // The config and everything below is only touched from the world thread (config load, game event
    // updates, world update, logins, chat commands). The reward hooks run on map threads too, so the
    // rates they read are atomics.
    Config config;
    bool worldReady = false;
    bool pendingEventUpdate = false;
    uint32 bonusTimer = 0;

    struct OriginalSchedule
    {
        time_t start;
        time_t end;
        uint32 occurence;
        uint32 length;
    };

    std::map<uint16, OriginalSchedule> originals;
    std::set<uint16> warnedEvents;

    std::atomic<float> activeRates[MAX_BONUS_STATS] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    float Rate(BonusStat stat)
    {
        return activeRates[stat].load(std::memory_order_relaxed);
    }

    // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
    // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
    // neither. Looking for both at compile time lets the module build on all of them.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    time_t Now()
    {
        return GameTime::GetGameTime().count();
    }

    // --- Parsing ---------------------------------------------------------------------------------

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    std::string Trim(std::string const& text)
    {
        size_t first = text.find_first_not_of(" \t");
        if (first == std::string::npos)
            return "";

        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }

    std::optional<Mode> ParseMode(std::string const& value)
    {
        std::string mode = Lower(value);
        if (mode.empty() || mode == "auto")
            return Mode::Auto;
        if (mode == "on" || mode == "1")
            return Mode::On;
        if (mode == "off" || mode == "0")
            return Mode::Off;
        return std::nullopt;
    }

    std::optional<bool> ParseBool(std::string const& value)
    {
        std::string flag = Lower(value);
        if (flag == "1" || flag == "yes" || flag == "true")
            return true;
        if (flag == "0" || flag == "no" || flag == "false")
            return false;
        return std::nullopt;
    }

    // "1w", "2d12h", "90m", or "0". Returns seconds.
    std::optional<uint32> ParseDuration(std::string const& value)
    {
        if (value == "0")
            return 0;

        uint64 total = 0;
        uint64 number = 0;
        bool hasNumber = false;
        bool hasUnit = false;

        for (char c : value)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
                continue;

            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                number = number * 10 + uint64(c - '0');
                hasNumber = true;
                if (number > std::numeric_limits<uint32>::max())
                    return std::nullopt;
                continue;
            }

            uint64 unit = 0;
            switch (std::tolower(static_cast<unsigned char>(c)))
            {
                case 'w': unit = WEEK; break;
                case 'd': unit = DAY; break;
                case 'h': unit = HOUR; break;
                case 'm': unit = MINUTE; break;
                default: return std::nullopt;
            }

            if (!hasNumber)
                return std::nullopt;

            total += number * unit;
            number = 0;
            hasNumber = false;
            hasUnit = true;
        }

        if (hasNumber || !hasUnit || total > std::numeric_limits<uint32>::max())
            return std::nullopt;

        return uint32(total);
    }

    // "2026-10-03" or "2026-10-03 18:00", in the worldserver's local time.
    std::optional<time_t> ParseDate(std::string const& value)
    {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
        char extra = 0;
        int fields = std::sscanf(value.c_str(), "%d-%d-%d %d:%d %c", &year, &month, &day, &hour, &minute, &extra);
        if (fields != 3 && fields != 5)
            return std::nullopt;

        if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31
            || hour < 0 || hour > 23 || minute < 0 || minute > 59)
            return std::nullopt;

        std::tm time = {};
        time.tm_year = year - 1900;
        time.tm_mon = month - 1;
        time.tm_mday = day;
        time.tm_hour = hour;
        time.tm_min = minute;
        time.tm_isdst = -1;

        time_t result = std::mktime(&time);
        if (result == time_t(-1))
            return std::nullopt;

        return result;
    }

    // "24" or "24, 70, 91"
    std::optional<std::vector<uint16>> ParseEventList(std::string const& value)
    {
        std::vector<uint16> ids;
        std::string buffer = value;
        for (char& c : buffer)
            if (c == ',')
                c = ' ';

        size_t pos = 0;
        while (pos < buffer.size())
        {
            size_t first = buffer.find_first_not_of(' ', pos);
            if (first == std::string::npos)
                break;

            size_t last = buffer.find(' ', first);
            Optional<uint16> id = Acore::StringTo<uint16>(buffer.substr(first, last - first));
            if (!id || !*id)
                return std::nullopt;

            ids.push_back(*id);
            pos = last;
        }

        return ids;
    }

    // HolidayControl.<Kind>.<key>.<field>: returns key and field, the field in lower case.
    std::optional<std::pair<std::string, std::string>> SplitKey(std::string const& option, std::string const& prefix)
    {
        std::string rest = option.substr(prefix.size());
        size_t dot = rest.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == rest.size())
            return std::nullopt;

        return std::make_pair(rest.substr(0, dot), Lower(rest.substr(dot + 1)));
    }

    // Shared by game event rules and bonus events. Returns false for a bad value, nullopt for an
    // unknown field.
    template <typename Rule>
    std::optional<bool> ParseScheduleField(Rule& rule, std::string const& field, std::string const& value)
    {
        if (field == "mode")
        {
            std::optional<Mode> mode = ParseMode(value);
            if (mode)
                rule.mode = *mode;
            return mode.has_value();
        }

        if (field == "name")
        {
            rule.name = value;
            return true;
        }

        if (field == "start" || field == "end")
        {
            if (value.empty())
                return true;

            std::optional<time_t> date = ParseDate(value);
            if (date)
                (field == "start" ? rule.start : rule.end) = *date;
            return date.has_value();
        }

        if (field == "length" || field == "repeat")
        {
            if (value.empty())
                return true;

            std::optional<uint32> duration = ParseDuration(value);
            if (!duration || (field == "length" && *duration < MINUTE))
                return false;

            (field == "length" ? rule.length : rule.repeat) = *duration;
            return true;
        }

        return std::nullopt;
    }

    void LogBadOption(std::string const& option, std::string const& value, char const* why)
    {
        LOG_ERROR("module", "HolidayControl: {} = \"{}\" {}, ignored.", option, value, why);
    }

    void LoadEventRule(Config& target, std::string const& option)
    {
        auto parts = SplitKey(option, EVENT_PREFIX);
        Optional<uint16> id = parts ? Acore::StringTo<uint16>(parts->first) : std::nullopt;
        if (!id || !*id)
        {
            LOG_ERROR("module", "HolidayControl: {} isn't HolidayControl.Event.<event id>.<setting>, ignored.", option);
            return;
        }

        std::string value = Trim(sConfigMgr->GetOption<std::string>(option, "", false));
        EventRule& rule = target.events[*id];
        std::string const& field = parts->second;

        if (field == "show")
        {
            rule.show = ParseBool(value);
            if (!rule.show && !value.empty())
                LogBadOption(option, value, "is not 0 or 1");
            return;
        }

        std::optional<bool> parsed = ParseScheduleField(rule, field, value);
        if (!parsed)
            LOG_ERROR("module", "HolidayControl: {} is not a setting (use Mode, Name, Show, Start, Length, Repeat, "
                "End), ignored.", option);
        else if (!*parsed)
            LogBadOption(option, value, "is not a valid value");
    }

    void LoadBonus(Config& target, std::string const& option)
    {
        auto parts = SplitKey(option, BONUS_PREFIX);
        if (!parts)
        {
            LOG_ERROR("module", "HolidayControl: {} isn't HolidayControl.Bonus.<name>.<setting>, ignored.", option);
            return;
        }

        std::string value = Trim(sConfigMgr->GetOption<std::string>(option, "", false));
        Bonus& bonus = target.bonuses[parts->first];
        std::string const& field = parts->second;

        if (field == "during")
        {
            std::optional<std::vector<uint16>> ids = ParseEventList(value);
            if (ids)
                bonus.during = std::move(*ids);
            else
                LogBadOption(option, value, "is not a list of game event ids");
            return;
        }

        for (uint8 stat = 0; stat < MAX_BONUS_STATS; ++stat)
        {
            if (field != BONUS_KEYS[stat])
                continue;

            Optional<float> rate = Acore::StringTo<float>(value);
            if (rate && *rate >= 0.0f && *rate <= 100.0f)
                bonus.rates[stat] = *rate;
            else
                LogBadOption(option, value, "is not a multiplier between 0 and 100");
            return;
        }

        std::optional<bool> parsed = ParseScheduleField(bonus, field, value);
        if (!parsed)
            LOG_ERROR("module", "HolidayControl: {} is not a setting (use Name, Mode, Start, Length, Repeat, End, "
                "During, XP, Reputation, Gold, Gathering, Drops), ignored.", option);
        else if (!*parsed)
            LogBadOption(option, value, "is not a valid value");
    }

    void FinishBonus(std::string const& key, Bonus& bonus)
    {
        if (bonus.name.empty())
            bonus.name = key;

        if (bonus.start)
        {
            if (!bonus.length)
                LOG_ERROR("module", "HolidayControl: bonus {} has a Start but no Length, so it only runs through "
                    "Mode or During.", key);
            else
                bonus.window = MakeWindow(*bonus.start, *bonus.length, bonus.repeat.value_or(0),
                    bonus.end.value_or(FAR_FUTURE));
        }
        else if (bonus.length || bonus.repeat || bonus.end)
            LOG_ERROR("module", "HolidayControl: bonus {} has Length, Repeat or End but no Start; they're ignored.", key);

        if (bonus.mode == Mode::Auto && !bonus.window.IsValid() && bonus.during.empty())
            LOG_WARN("module", "HolidayControl: bonus {} has no Start/Length, During or Mode = on, so it never runs.", key);
    }

    void LoadConfig()
    {
        Config fresh;
        fresh.enabled = sConfigMgr->GetOption<bool>("HolidayControl.Enable", true);
        fresh.announce = sConfigMgr->GetOption<bool>("HolidayControl.Announce", true);
        fresh.loginMessage = sConfigMgr->GetOption<bool>("HolidayControl.LoginMessage", true);
        fresh.upcomingDays = sConfigMgr->GetOption<uint32>("HolidayControl.UpcomingDays", 30);

        for (std::string const& option : sConfigMgr->GetKeysByString(EVENT_PREFIX))
            LoadEventRule(fresh, option);

        for (std::string const& option : sConfigMgr->GetKeysByString(BONUS_PREFIX))
            LoadBonus(fresh, option);

        for (auto& [key, bonus] : fresh.bonuses)
        {
            FinishBonus(key, bonus);

            // Keep the running state through a reload, so only real changes get announced.
            auto old = config.bonuses.find(key);
            if (old != config.bonuses.end())
                bonus.active = old->second.active;
        }

        config = std::move(fresh);

        uint32 controlled = uint32(std::count_if(config.events.begin(), config.events.end(),
            [](auto const& entry) { return entry.second.Controls(); }));
        LOG_INFO("module", "HolidayControl: {} game events controlled by the config, {} bonus events.", controlled,
            config.bonuses.size());
    }

    // --- Game events -----------------------------------------------------------------------------

    GameEventMgr::GameEventDataMap const& Events()
    {
        return sGameEventMgr->GetEventMap();
    }

    // The core only hands out the event list read-only, but it's a plain member of the singleton, and
    // the schedule fields are exactly what the core reads on its next check.
    GameEventData& MutableEvent(uint16 eventId)
    {
        return const_cast<GameEventMgr::GameEventDataMap&>(Events())[eventId];
    }

    EventRule const* FindRule(uint16 eventId)
    {
        if (!config.enabled)
            return nullptr;

        auto itr = config.events.find(eventId);
        return itr != config.events.end() ? &itr->second : nullptr;
    }

    EventRule const* FindControllingRule(uint16 eventId)
    {
        EventRule const* rule = FindRule(eventId);
        return rule && rule->Controls() ? rule : nullptr;
    }

    bool IsForcedOn(uint16 eventId)
    {
        EventRule const* rule = FindControllingRule(eventId);
        return rule && rule->mode == Mode::On && originals.count(eventId);
    }

    bool IsForcedOff(uint16 eventId)
    {
        EventRule const* rule = FindControllingRule(eventId);
        return rule && rule->mode == Mode::Off && originals.count(eventId);
    }

    void RestoreSchedule(GameEventData& data, OriginalSchedule const& original)
    {
        data.Start = original.start;
        data.End = original.end;
        data.Occurence = original.occurence;
        data.Length = original.length;
    }

    // Called by the core right before it checks whether the event should be running.
    void ApplyRule(uint16 eventId)
    {
        if (eventId >= Events().size())
            return;

        GameEventData& data = MutableEvent(eventId);
        EventRule const* rule = FindControllingRule(eventId);
        auto original = originals.find(eventId);

        if (!rule)
        {
            if (original != originals.end())
            {
                RestoreSchedule(data, original->second);
                originals.erase(original);
            }
            return;
        }

        if (original == originals.end())
        {
            if (data.State != GAMEEVENT_NORMAL || !data.isValid())
            {
                if (warnedEvents.insert(eventId).second)
                    LOG_ERROR("module", "HolidayControl: game event {} {}, so the config can't control it.", eventId,
                        data.isValid() ? "(" + data.Description + ") is a world event that runs on its own states"
                                       : "doesn't exist");
                return;
            }

            original = originals.emplace(eventId, OriginalSchedule{ data.Start, data.End, data.Occurence, data.Length }).first;
        }

        OriginalSchedule const& own = original->second;

        switch (rule->mode)
        {
            case Mode::On:
                // Every moment is inside a window that never ends.
                data.Start = 1;
                data.End = FAR_FUTURE;
                data.Occurence = FORCED_ON_MINUTES;
                data.Length = FORCED_ON_MINUTES;
                return;
            case Mode::Off:
                RestoreSchedule(data, own);
                data.End = 0;
                return;
            case Mode::Auto:
                break;
        }

        time_t start = rule->start.value_or(own.start);
        uint32 length = rule->length.value_or(own.length * MINUTE);
        uint32 repeat = rule->repeat.value_or(own.occurence * MINUTE);
        time_t end = rule->end.value_or(rule->start ? FAR_FUTURE : own.end);
        Window window = MakeWindow(start, length, repeat, end);

        data.Start = window.start;
        data.End = window.end;
        data.Length = window.length / MINUTE;
        data.Occurence = window.period / MINUTE;
    }

    Window EventWindow(GameEventData const& data)
    {
        Window window;
        window.start = data.Start;
        window.length = data.Length * MINUTE;
        window.period = data.Occurence * MINUTE;
        window.end = data.End;
        return window;
    }

    // Holidays are shown and announced; other events only when the config asks for them.
    bool IsShown(uint16 eventId)
    {
        if (eventId >= Events().size())
            return false;

        GameEventData const& data = Events()[eventId];
        if (data.State != GAMEEVENT_NORMAL || !data.isValid())
            return false;

        if (EventRule const* rule = FindRule(eventId))
        {
            if (rule->show)
                return *rule->show;

            if (rule->Controls() || !rule->name.empty())
                return true;
        }

        return data.HolidayId != HOLIDAY_NONE && data.HolidayStage == sGameEventMgr->GetHolidayMainStage(data.HolidayId);
    }

    std::string EventName(uint16 eventId)
    {
        EventRule const* rule = FindRule(eventId);
        if (rule && !rule->name.empty())
            return rule->name;

        return Events()[eventId].Description;
    }

    // When the running event ends, or 0 if it doesn't (forced on).
    time_t EventEnd(uint16 eventId, time_t now)
    {
        if (IsForcedOn(eventId))
            return 0;

        Window window = EventWindow(Events()[eventId]);
        return window.Contains(now) ? window.CurrentEnd(now) : now;
    }

    time_t EventNextStart(uint16 eventId, time_t now)
    {
        if (IsForcedOn(eventId) || IsForcedOff(eventId))
            return 0;

        return EventWindow(Events()[eventId]).NextStart(now);
    }

    // --- Text ------------------------------------------------------------------------------------

    std::string FormatSpan(time_t seconds)
    {
        if (seconds < MINUTE)
            return "less than a minute";

        uint32 days = uint32(seconds / DAY);
        uint32 hours = uint32(seconds % DAY / HOUR);
        uint32 minutes = uint32(seconds % HOUR / MINUTE);

        if (days)
            return hours ? Acore::StringFormat("{}d {}h", days, hours) : Acore::StringFormat("{}d", days);
        if (hours)
            return minutes ? Acore::StringFormat("{}h {}m", hours, minutes) : Acore::StringFormat("{}h", hours);
        return Acore::StringFormat("{}m", minutes);
    }

    std::string FormatDate(time_t time)
    {
        std::tm local = Acore::Time::TimeBreakdown(time);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%b %d, %H:%M", &local);
        return buffer;
    }

    std::string EndText(time_t end, time_t now)
    {
        return end ? "ends in " + FormatSpan(end - now) : "on until further notice";
    }

    std::string RatesText(Bonus const& bonus)
    {
        std::string text;
        for (uint8 stat = 0; stat < MAX_BONUS_STATS; ++stat)
        {
            if (bonus.rates[stat] == 1.0f)
                continue;

            if (!text.empty())
                text += ", ";

            text += Acore::StringFormat("{:g}x {}", bonus.rates[stat], BONUS_NAMES[stat]);
        }

        return text;
    }

    void Broadcast(std::string const& text)
    {
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session || IsBotSession(session))
                continue;

            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld())
                continue;

            ChatHandler(session).SendSysMessage(text);
        }
    }

    void AnnounceEvent(uint16 eventId, bool started)
    {
        if (!worldReady || !config.enabled || !config.announce || !IsShown(eventId))
            return;

        std::string name = EventName(eventId);
        if (!started)
        {
            Broadcast(HOLIDAY_TAG + name + " has ended.");
            return;
        }

        time_t now = Now();
        time_t end = EventEnd(eventId, now);
        Broadcast(HOLIDAY_TAG + name + " has begun! " + (end ? "It ends in " + FormatSpan(end - now) + "." : ""));
    }

    // --- Bonus events ----------------------------------------------------------------------------

    bool IsBonusActive(Bonus const& bonus, time_t now)
    {
        switch (bonus.mode)
        {
            case Mode::On: return true;
            case Mode::Off: return false;
            case Mode::Auto: break;
        }

        if (bonus.window.Contains(now))
            return true;

        return std::any_of(bonus.during.begin(), bonus.during.end(),
            [](uint16 eventId) { return sGameEventMgr->IsActiveEvent(eventId); });
    }

    // When the running bonus ends, or 0 if it doesn't.
    time_t BonusEnd(Bonus const& bonus, time_t now)
    {
        if (bonus.mode == Mode::On)
            return 0;

        time_t end = bonus.window.Contains(now) ? bonus.window.CurrentEnd(now) : now;
        for (uint16 eventId : bonus.during)
        {
            if (!sGameEventMgr->IsActiveEvent(eventId))
                continue;

            time_t eventEnd = EventEnd(eventId, now);
            if (!eventEnd)
                return 0;

            end = std::max(end, eventEnd);
        }

        return end;
    }

    time_t BonusNextStart(Bonus const& bonus, time_t now)
    {
        if (bonus.mode != Mode::Auto)
            return 0;

        time_t next = bonus.window.NextStart(now);
        for (uint16 eventId : bonus.during)
        {
            time_t eventNext = EventNextStart(eventId, now);
            if (eventNext && (!next || eventNext < next))
                next = eventNext;
        }

        return next;
    }

    // Overlapping bonuses don't stack: each stat uses the highest rate among the running bonuses
    // that change it.
    void UpdateRates()
    {
        for (uint8 stat = 0; stat < MAX_BONUS_STATS; ++stat)
        {
            std::optional<float> best;
            for (auto const& [key, bonus] : config.bonuses)
                if (bonus.active && bonus.rates[stat] != 1.0f)
                    best = std::max(best.value_or(0.0f), bonus.rates[stat]);

            activeRates[stat].store(best.value_or(1.0f), std::memory_order_relaxed);
        }
    }

    void UpdateBonuses(bool announce)
    {
        time_t now = Now();
        for (auto& [key, bonus] : config.bonuses)
        {
            bool active = config.enabled && IsBonusActive(bonus, now);
            if (active == bonus.active)
                continue;

            bonus.active = active;
            LOG_INFO("module", "HolidayControl: bonus event {} {}.", bonus.name, active ? "started" : "ended");

            if (!announce || !config.announce)
                continue;

            if (!active)
            {
                Broadcast(BONUS_TAG + bonus.name + " has ended.");
                continue;
            }

            std::string text = BONUS_TAG + bonus.name + " has begun";
            std::string rates = RatesText(bonus);
            if (!rates.empty())
                text += ": " + rates;
            text += ".";

            if (time_t end = BonusEnd(bonus, now))
                text += " It ends in " + FormatSpan(end - now) + ".";

            Broadcast(text);
        }

        UpdateRates();
    }

    // --- Listings --------------------------------------------------------------------------------

    struct Line
    {
        time_t when;
        std::string text;
    };

    std::vector<std::string> RunningLines(time_t now)
    {
        std::vector<std::string> lines;
        for (uint16 eventId : sGameEventMgr->GetActiveEventList())
            if (IsShown(eventId))
                lines.push_back(EventName(eventId) + ": " + EndText(EventEnd(eventId, now), now));

        for (auto const& [key, bonus] : config.bonuses)
        {
            if (!bonus.active)
                continue;

            std::string rates = RatesText(bonus);
            lines.push_back(bonus.name + (rates.empty() ? "" : " (" + rates + ")") + ": "
                + EndText(BonusEnd(bonus, now), now));
        }

        return lines;
    }

    std::vector<Line> UpcomingLines(time_t now)
    {
        std::vector<Line> lines;
        time_t horizon = now + time_t(config.upcomingDays) * DAY;

        for (uint16 eventId = 1; eventId < Events().size(); ++eventId)
        {
            if (sGameEventMgr->IsActiveEvent(eventId) || !IsShown(eventId))
                continue;

            time_t next = EventNextStart(eventId, now);
            if (next && next <= horizon)
                lines.push_back({ next, EventName(eventId) });
        }

        for (auto const& [key, bonus] : config.bonuses)
        {
            if (bonus.active)
                continue;

            time_t next = BonusNextStart(bonus, now);
            if (next && next <= horizon)
                lines.push_back({ next, bonus.name });
        }

        std::sort(lines.begin(), lines.end(), [](Line const& a, Line const& b) { return a.when < b.when; });
        for (Line& line : lines)
            line.text += ": in " + FormatSpan(line.when - now) + " (" + FormatDate(line.when) + ")";

        return lines;
    }

    std::string ModeText(Mode mode)
    {
        switch (mode)
        {
            case Mode::On: return "forced on";
            case Mode::Off: return "forced off";
            default: return "rescheduled";
        }
    }

    bool Matches(std::string const& name, std::string const& filter)
    {
        return filter.empty() || Lower(name).find(filter) != std::string::npos;
    }
}

class HolidayControlWorldScript : public WorldScript
{
public:
    HolidayControlWorldScript() : WorldScript("HolidayControlWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // The game event system isn't running yet on the first load; it picks the rules up when it
        // starts. After .reload config, recheck every event on the next world update.
        if (reload)
            pendingEventUpdate = true;
    }

    void OnStartup() override
    {
        worldReady = true;
        UpdateBonuses(false);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!worldReady)
            return;

        if (pendingEventUpdate)
        {
            pendingEventUpdate = false;
            sWorld->ForceGameEventUpdate();
        }

        bonusTimer += diff;
        if (bonusTimer < IN_MILLISECONDS)
            return;

        bonusTimer = 0;
        UpdateBonuses(true);
    }
};

class HolidayControlGameEventScript : public GameEventScript
{
public:
    HolidayControlGameEventScript() : GameEventScript("HolidayControlGameEventScript", {
        GAMEEVENTHOOK_ON_START,
        GAMEEVENTHOOK_ON_STOP,
        GAMEEVENTHOOK_ON_EVENT_CHECK,
    }) { }

    void OnEventCheck(uint16 eventId) override
    {
        ApplyRule(eventId);
    }

    void OnStart(uint16 eventId) override
    {
        AnnounceEvent(eventId, true);
    }

    void OnStop(uint16 eventId) override
    {
        AnnounceEvent(eventId, false);
    }
};

class HolidayControlPlayerScript : public PlayerScript
{
public:
    HolidayControlPlayerScript() : PlayerScript("HolidayControlPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_GIVE_REPUTATION,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_ON_UPDATE_GATHERING_SKILL,
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!config.enabled || !config.loginMessage || !player || IsBotSession(player->GetSession()))
            return;

        time_t now = Now();
        std::vector<std::string> parts;
        for (uint16 eventId : sGameEventMgr->GetActiveEventList())
        {
            if (!IsShown(eventId))
                continue;

            time_t end = EventEnd(eventId, now);
            parts.push_back(EventName(eventId) + (end ? " (" + FormatSpan(end - now) + " left)" : ""));
        }

        for (auto const& [key, bonus] : config.bonuses)
        {
            if (!bonus.active)
                continue;

            time_t end = BonusEnd(bonus, now);
            parts.push_back(bonus.name + (end ? " (" + FormatSpan(end - now) + " left)" : ""));
        }

        if (parts.empty())
            return;

        std::string text = std::string(HOLIDAY_TAG) + "Happening now: ";
        for (size_t i = 0; i < parts.size(); ++i)
            text += (i ? ", " : "") + parts[i];
        text += ". Type .holidays for more.";

        ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    void OnPlayerGiveXP(Player* /*player*/, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        float rate = Rate(BONUS_XP);
        if (rate != 1.0f && amount)
            amount = uint32(std::min<double>(std::lround(double(amount) * rate), std::numeric_limits<uint32>::max()));
    }

    void OnPlayerGiveReputation(Player* /*player*/, int32 /*factionID*/, float& amount, ReputationSource /*repSource*/) override
    {
        // Only gains: a bonus event shouldn't make reputation losses bigger.
        float rate = Rate(BONUS_REPUTATION);
        if (rate != 1.0f && amount > 0.0f)
            amount *= rate;
    }

    void OnPlayerBeforeLootMoney(Player* /*player*/, Loot* loot) override
    {
        float rate = Rate(BONUS_GOLD);
        if (rate != 1.0f && loot && loot->gold)
            loot->gold = uint32(std::min<double>(std::lround(double(loot->gold) * rate), std::numeric_limits<uint32>::max()));
    }

    void OnPlayerUpdateGatheringSkill(Player* /*player*/, uint32 /*skillId*/, uint32 /*current*/, uint32 /*gray*/,
        uint32 /*green*/, uint32 /*yellow*/, uint32& gain) override
    {
        float rate = Rate(BONUS_GATHERING);
        if (rate != 1.0f && gain)
            gain = uint32(std::max<long>(rate > 0.0f ? 1 : 0, std::lround(double(gain) * rate)));
    }
};

class HolidayControlGlobalScript : public GlobalScript
{
public:
    HolidayControlGlobalScript() : GlobalScript("HolidayControlGlobalScript", { GLOBALHOOK_ON_ITEM_ROLL }) { }

    // Raises the chance of loot entries that roll on their own, including references (the world
    // drop tables). Grouped entries are left alone: their chances share one roll, so raising them
    // only shifts which item of the group drops.
    bool OnItemRoll(Player const* /*player*/, LootStoreItem const* item, float& chance, Loot& /*loot*/,
        LootStore const& store) override
    {
        float rate = Rate(BONUS_DROPS);
        if (rate == 1.0f || !item || item->groupid || chance <= 0.0f || chance >= 100.0f)
            return true;

        if (&store != &LootTemplates_Creature && &store != &LootTemplates_Gameobject
            && &store != &LootTemplates_Fishing && &store != &LootTemplates_Skinning
            && &store != &LootTemplates_Pickpocketing)
            return true;

        chance = std::min(100.0f, chance * rate);
        return true;
    }
};

class HolidayControlCommandScript : public CommandScript
{
public:
    HolidayControlCommandScript() : CommandScript("HolidayControlCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable holidaysTable =
        {
            { "",     HandleHolidays, SEC_PLAYER,     Console::Yes },
            { "list", HandleList,     SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "holidays", holidaysTable },
        };

        return commandTable;
    }

    static bool HandleHolidays(ChatHandler* handler)
    {
        if (!config.enabled)
        {
            handler->SendSysMessage("Holiday control is switched off on this server.");
            return true;
        }

        time_t now = Now();
        std::vector<std::string> running = RunningLines(now);
        if (running.empty())
            handler->SendSysMessage("No holiday or bonus event is running right now.");
        else
        {
            handler->SendSysMessage("|cffffd000Happening now:|r");
            for (std::string const& line : running)
                handler->SendSysMessage("  " + line);
        }

        std::vector<Line> upcoming = UpcomingLines(now);
        if (!upcoming.empty())
        {
            handler->PSendSysMessage("|cffffd000Coming up in the next {} days:|r", config.upcomingDays);
            for (Line const& line : upcoming)
                handler->SendSysMessage("  " + line.text);
        }

        return true;
    }

    // Every scheduled game event with its id, so you can find the ids to put in the config.
    static bool HandleList(ChatHandler* handler, Tail filterText)
    {
        std::string filter = Lower(Trim(std::string(filterText)));
        time_t now = Now();
        uint32 shown = 0;

        for (uint16 eventId = 1; eventId < Events().size(); ++eventId)
        {
            GameEventData const& data = Events()[eventId];
            if (!data.isValid() || (!Matches(data.Description, filter) && !Matches(EventName(eventId), filter)))
                continue;

            // The AQ War Effort alone has 60 world event stages; only list them when asked for.
            if (data.State != GAMEEVENT_NORMAL && filter.empty())
                continue;

            std::string state;
            if (data.State != GAMEEVENT_NORMAL)
                state = "world event, not controllable";
            else if (sGameEventMgr->IsActiveEvent(eventId))
                state = "|cff00ff00running|r, " + EndText(EventEnd(eventId, now), now);
            else if (time_t next = EventNextStart(eventId, now))
                state = "starts in " + FormatSpan(next - now) + " (" + FormatDate(next) + ")";
            else
                state = "not scheduled";

            if (EventRule const* rule = FindControllingRule(eventId))
                state += ", |cffff8000" + ModeText(rule->mode) + " by config|r";

            handler->PSendSysMessage("[{}] {}: {}", eventId, EventName(eventId), state);
            ++shown;
        }

        for (auto const& [key, bonus] : config.bonuses)
        {
            if (!Matches(bonus.name, filter) && !Matches(key, filter))
                continue;

            std::string state;
            if (bonus.active)
                state = "|cff00ff00running|r, " + EndText(BonusEnd(bonus, now), now);
            else if (time_t next = BonusNextStart(bonus, now))
                state = "starts in " + FormatSpan(next - now) + " (" + FormatDate(next) + ")";
            else
                state = "not scheduled";

            handler->PSendSysMessage("[Bonus {}] {}: {}", key, bonus.name, state);
            ++shown;
        }

        if (!shown)
            handler->SendSysMessage("No event matches.");

        return true;
    }
};

void AddHolidayControlScripts()
{
    new HolidayControlWorldScript();
    new HolidayControlGameEventScript();
    new HolidayControlPlayerScript();
    new HolidayControlGlobalScript();
    new HolidayControlCommandScript();
}
