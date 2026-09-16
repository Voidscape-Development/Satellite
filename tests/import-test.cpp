/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// Pins the DistroAV compatibility mapping.
//
// Scanning a scene collection needs a running OBS, so that part is not covered here. What is
// covered is the translation that carries DistroAV's magic numbers - values read out of its
// ndi-source.cpp rather than documented anywhere - because a silent change to them would
// quietly downgrade every imported source instead of failing loudly.

#include "config/config.hpp"
#include "obs/distroav-import.hpp"
#include "transport/transport.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" void obs_log(int, const char *, ...) {}

extern "C" {
const char *PLUGIN_NAME = "satellite";
const char *PLUGIN_VERSION = "0.1.0";
}

namespace satellite {
Config &Config::instance()
{
	static Config config;
	return config;
}
void Config::load() {}
void Config::save() const {}
} // namespace satellite

using namespace satellite;

static int g_failures = 0;

static void check(bool condition, const char *what)
{
	printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++g_failures;
}

int main()
{
	printf("== DistroAV bandwidth mapping ==\n");

	// PROP_BW_HIGHEST is 0 in DistroAV.
	check(distroav_quality_from_bandwidth(0) == Quality::Default, "highest becomes full quality");

	// PROP_BW_LOWEST is 1 - the low-bandwidth proxy feed, which is Satellite's preview mode.
	check(distroav_quality_from_bandwidth(1) == Quality::PreviewOnly, "lowest becomes preview");

	// PROP_BW_AUDIO_ONLY is 2.
	check(distroav_quality_from_bandwidth(2) == Quality::AudioOnly, "audio-only carries across");

	// PROP_BW_UNDEFINED is -1, and a future DistroAV could add values we have never seen.
	// Both must land on full quality: guessing lower would silently degrade a feed, and the
	// user would have no idea why their camera looks soft after importing.
	check(distroav_quality_from_bandwidth(-1) == Quality::Default, "undefined falls back to full quality");
	check(distroav_quality_from_bandwidth(99) == Quality::Default, "unknown values fall back to full quality");

	printf("== quality round trip ==\n");

	// The mapping is stored in settings as a string, so the ids have to survive the trip.
	for (int bandwidth : {-1, 0, 1, 2, 99}) {
		const Quality mapped = distroav_quality_from_bandwidth(bandwidth);
		Quality parsed = Quality::High;
		const bool ok = quality_from_id(quality_id(mapped), parsed) && parsed == mapped;
		if (!ok) {
			printf("  FAIL bandwidth %d did not round trip through its id\n", bandwidth);
			++g_failures;
		}
	}
	check(true, "every mapped quality round trips through its settings id");

	printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures,
	       g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
