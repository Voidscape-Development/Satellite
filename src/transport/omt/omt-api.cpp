/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "transport/omt/omt-api.hpp"

namespace satellite {

bool OmtApi::bind(const LibraryHandle &library, std::string &missing_symbol)
{
#define SATELLITE_BIND(member, symbol)                        \
	do {                                                  \
		if (!library.bind(member, symbol)) {          \
			missing_symbol = symbol;              \
			return false;                         \
		}                                             \
	} while (false)

	SATELLITE_BIND(discovery_getaddresses, "omt_discovery_getaddresses");

	SATELLITE_BIND(receive_create, "omt_receive_create");
	SATELLITE_BIND(receive_destroy, "omt_receive_destroy");
	SATELLITE_BIND(receive, "omt_receive");
	SATELLITE_BIND(receive_settally, "omt_receive_settally");
	SATELLITE_BIND(receive_setsuggestedquality, "omt_receive_setsuggestedquality");
	SATELLITE_BIND(receive_getvideostatistics, "omt_receive_getvideostatistics");

	SATELLITE_BIND(send_create, "omt_send_create");
	SATELLITE_BIND(send_destroy, "omt_send_destroy");
	SATELLITE_BIND(send, "omt_send");
	SATELLITE_BIND(send_connections, "omt_send_connections");
	SATELLITE_BIND(send_gettally, "omt_send_gettally");
	SATELLITE_BIND(send_getvideostatistics, "omt_send_getvideostatistics");

	SATELLITE_BIND(settings_set_string, "omt_settings_set_string");
	SATELLITE_BIND(settings_set_integer, "omt_settings_set_integer");
	SATELLITE_BIND(shutdown, "omt_shutdown");

#undef SATELLITE_BIND

	missing_symbol.clear();
	return true;
}

} // namespace satellite
