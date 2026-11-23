#pragma once

// A84b §2 (TASK-A84b-CLOSE-AUTOTUNE-GAPS-2026-11-23): the ONE-TIME settings-file
// migration behind A84's flipped autoTune default, as a pure predicate.
//
// It used to live inline in Settings::FromJson (pc/streamer/src/settings.cpp). That
// file is compiled into rincast-streamer only - rincast-core-tests links
// rincast-core plus this header, and core_tests.cpp never names FromJson,
// ToJson or SettingsStore - so the judgement a user's existing settings.json is
// put through had no test that could fail. It moved here so it can.
//
// The judgement itself: a settings file is saved WHOLE (SettingsStore::SaveLocked
// dumps every key), so a file written before A84 carries an explicit
// "autoTune": false that proves nothing about what the user wanted - it is the old
// default, not a choice. Only a file stamped with a schema at or above the
// threshold may be trusted to hold a decision, because from that build on every
// save writes the stamp and every UI read-modify-write re-stamps it.
//
// The stamp STARTS AT 1, not 2. There is no v1 schema anywhere in this repository:
// `git grep -i schema` over HEAD returns nothing outside this card's own lines, and
// `git log -S settingsSchema` has no commits - the key is introduced here, so an
// unstamped file and a "v1" file are the same file. A 2 would be a magic number
// with no v1 to tell it apart from, and it would make the threshold and the current
// stamp two names for one thing for no reason a reader could check.
//
// What stays in settings.cpp (and what this header therefore cannot cover): reading
// the key off the parsed JSON. That read is deliberately type-checked -
// `it != j.end() && it->is_number()` before `get<int>()`, because FromJson also runs
// on the app thread over a hand-edited file and a throw there is fatal - and it is
// nlohmann-specific, so it cannot leave the streamer target. Its whole contract is
// to produce the sentinel below, which is why the sentinel is declared here: absent
// key, wrong type, or a garbage number all reach this predicate as 0 and read as
// "no stamp", so they migrate.
#include <cstdint>

namespace rc {

// A file this predicate has never seen (absent key, or one that was not a number).
inline constexpr int kSettingsSchemaUnstamped = 0;
// From this schema up, a file's stored autoTune value is a user decision.
inline constexpr int kSettingsSchemaAutoTuneOn = 1;
// What this build stamps its own file with. Equal to the threshold today: this is
// the first stamped schema, so the first file written is the first one trusted.
// They part company the next time a migration lands - keep them separate.
inline constexpr int kCurrentSettingsSchema = kSettingsSchemaAutoTuneOn;

// True when the autoTune default must be applied over whatever the file says.
inline bool MigrateAutoTuneOn(int savedSchema) { return savedSchema < kSettingsSchemaAutoTuneOn; }

}  // namespace rc
