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

#include "transport/transport.hpp"

#include "transport/ndi/ndi-backend.hpp"
#include "transport/omt/omt-backend.hpp"

#include <cstring>

#include <plugin-support.h>
#include <obs-module.h>

namespace satellite {

const Protocol kAllProtocols[2] = {Protocol::NDI, Protocol::OMT};

const char *protocol_id(Protocol protocol)
{
	switch (protocol) {
	case Protocol::NDI:
		return "ndi";
	case Protocol::OMT:
		return "omt";
	}
	return "ndi";
}

const char *protocol_display_name(Protocol protocol)
{
	switch (protocol) {
	case Protocol::NDI:
		return "NDI";
	case Protocol::OMT:
		return "OMT";
	}
	return "NDI";
}

bool protocol_from_id(const char *id, Protocol &out)
{
	if (!id)
		return false;
	for (Protocol protocol : kAllProtocols) {
		if (strcmp(id, protocol_id(protocol)) == 0) {
			out = protocol;
			return true;
		}
	}
	return false;
}

const char *quality_id(Quality quality)
{
	switch (quality) {
	case Quality::Default:
		return "default";
	case Quality::Low:
		return "low";
	case Quality::Medium:
		return "medium";
	case Quality::High:
		return "high";
	case Quality::PreviewOnly:
		return "preview";
	case Quality::AudioOnly:
		return "audio_only";
	}
	return "default";
}

bool quality_from_id(const char *id, Quality &out)
{
	if (!id)
		return false;
	static const Quality all[] = {Quality::Default, Quality::Low,         Quality::Medium,
				      Quality::High,    Quality::PreviewOnly, Quality::AudioOnly};
	for (Quality quality : all) {
		if (strcmp(id, quality_id(quality)) == 0) {
			out = quality;
			return true;
		}
	}
	return false;
}

CapturedFrame::~CapturedFrame()
{
	release();
}

CapturedFrame &CapturedFrame::operator=(CapturedFrame &&other) noexcept
{
	if (this != &other) {
		release();
		move_from(other);
	}
	return *this;
}

void CapturedFrame::move_from(CapturedFrame &other) noexcept
{
	owner_ = other.owner_;
	type_ = other.type_;
	video = other.video;
	audio = other.audio;
	metadata = std::move(other.metadata);

	other.owner_ = nullptr;
	other.type_ = FrameType::None;
}

void CapturedFrame::release()
{
	if (owner_ && type_ != FrameType::None)
		owner_->release_frame(type_);
	owner_ = nullptr;
	type_ = FrameType::None;
}

namespace {

std::unique_ptr<IBackend> g_ndi_backend;
std::unique_ptr<IBackend> g_omt_backend;

} // namespace

void register_backends()
{
	g_ndi_backend = create_ndi_backend();
	g_omt_backend = create_omt_backend();

	for (IBackend *backend : all_backends()) {
		if (backend->load()) {
			obs_log(LOG_INFO, "%s runtime loaded (%s)", protocol_display_name(backend->protocol()),
				backend->runtime_version().c_str());
		} else {
			obs_log(LOG_INFO, "%s unavailable: %s", protocol_display_name(backend->protocol()),
				backend->unavailable_reason().c_str());
		}
	}
}

void unregister_backends()
{
	for (IBackend *backend : all_backends())
		backend->unload();

	g_ndi_backend.reset();
	g_omt_backend.reset();
}

IBackend *backend_for(Protocol protocol)
{
	switch (protocol) {
	case Protocol::NDI:
		return g_ndi_backend.get();
	case Protocol::OMT:
		return g_omt_backend.get();
	}
	return nullptr;
}

std::vector<IBackend *> all_backends()
{
	std::vector<IBackend *> backends;
	for (Protocol protocol : kAllProtocols) {
		if (IBackend *backend = backend_for(protocol))
			backends.push_back(backend);
	}
	return backends;
}

} // namespace satellite
