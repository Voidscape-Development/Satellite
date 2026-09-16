/*
Satellite - NDI and OMT transport for OBS Studio
Copyright (C) 2026 Voidscape Development

SPDX-License-Identifier: GPL-2.0-or-later
*/

// A fake NDI 6 runtime, built against Satellite's vendored SDK headers.
//
// It exports NDIlib_v6_load() and fills in only the entry points Satellite actually uses,
// so the loader, the discovery path and the frame conversion can be exercised without the
// proprietary runtime installed. Building this against the same header Satellite compiles
// against is the point: if Satellite's use of the ABI is wrong, this catches it.

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <Processing.NDI.Lib.h>

namespace {

struct FakeFind {
	NDIlib_source_t sources[2];
};

struct FakeRecv {
	int capture_count = 0;
	NDIlib_recv_bandwidth_e bandwidth = NDIlib_recv_bandwidth_highest;
	NDIlib_recv_color_format_e color_format = NDIlib_recv_color_format_UYVY_BGRA;
	NDIlib_tally_t tally = {};
	uint8_t *buffer = nullptr;
	float *audio = nullptr;
};

bool fake_initialize(void)
{
	return true;
}
void fake_destroy(void) {}
const char *fake_version(void)
{
	return "FAKE NDI 6.0.0";
}
bool fake_is_supported_CPU(void)
{
	return true;
}

NDIlib_find_instance_t fake_find_create_v2(const NDIlib_find_create_t *)
{
	auto *find = new FakeFind();
	find->sources[0].p_ndi_name = "STUDIO-PC (Camera 1)";
	find->sources[0].p_url_address = "192.168.1.10:5961";
	find->sources[1].p_ndi_name = "STUDIO-PC (Camera 2)";
	find->sources[1].p_url_address = "192.168.1.10:5962";
	return reinterpret_cast<NDIlib_find_instance_t>(find);
}

void fake_find_destroy(NDIlib_find_instance_t instance)
{
	delete reinterpret_cast<FakeFind *>(instance);
}

bool fake_find_wait_for_sources(NDIlib_find_instance_t, uint32_t)
{
	return true;
}

const NDIlib_source_t *fake_find_get_current_sources(NDIlib_find_instance_t instance, uint32_t *count)
{
	auto *find = reinterpret_cast<FakeFind *>(instance);
	*count = 2;
	return find->sources;
}

NDIlib_recv_instance_t fake_recv_create_v3(const NDIlib_recv_create_v3_t *settings)
{
	auto *recv = new FakeRecv();
	if (settings) {
		recv->bandwidth = settings->bandwidth;
		recv->color_format = settings->color_format;
	}
	return reinterpret_cast<NDIlib_recv_instance_t>(recv);
}

void fake_recv_destroy(NDIlib_recv_instance_t instance)
{
	auto *recv = reinterpret_cast<FakeRecv *>(instance);
	free(recv->buffer);
	free(recv->audio);
	delete recv;
}

// Which pixel format each capture hands back, so the plane maths gets exercised for every
// branch Satellite claims to support.
NDIlib_FourCC_video_type_e format_for_step(int step)
{
	switch (step % 5) {
	case 0:
		return NDIlib_FourCC_video_type_UYVY;
	case 1:
		return NDIlib_FourCC_video_type_BGRA;
	case 2:
		return NDIlib_FourCC_video_type_NV12;
	case 3:
		return NDIlib_FourCC_video_type_I420;
	default:
		return NDIlib_FourCC_video_type_YV12;
	}
}

NDIlib_frame_type_e fake_recv_capture_v3(NDIlib_recv_instance_t instance, NDIlib_video_frame_v2_t *video,
					 NDIlib_audio_frame_v3_t *audio, NDIlib_metadata_frame_t *, uint32_t)
{
	auto *recv = reinterpret_cast<FakeRecv *>(instance);
	const int step = recv->capture_count++;

	// Every third capture is audio, the rest video.
	if (step % 3 == 2) {
		if (!audio)
			return NDIlib_frame_type_none;

		const int channels = 2;
		const int samples = 480;
		if (!recv->audio)
			recv->audio = static_cast<float *>(calloc(channels * samples, sizeof(float)));

		audio->sample_rate = 48000;
		audio->no_channels = channels;
		audio->no_samples = samples;
		audio->timecode = 0;
		audio->timestamp = 1000000LL * (step + 1);
		audio->FourCC = NDIlib_FourCC_audio_type_FLTP;
		audio->p_data = reinterpret_cast<uint8_t *>(recv->audio);
		audio->channel_stride_in_bytes = samples * static_cast<int>(sizeof(float));
		return NDIlib_frame_type_audio;
	}

	if (!video)
		return NDIlib_frame_type_none;

	const int xres = 1920;
	const int yres = 1080;
	const NDIlib_FourCC_video_type_e fourcc = format_for_step(step);

	int stride = 0;
	size_t bytes = 0;
	switch (fourcc) {
	case NDIlib_FourCC_video_type_UYVY:
		stride = xres * 2;
		bytes = static_cast<size_t>(stride) * yres;
		break;
	case NDIlib_FourCC_video_type_BGRA:
		stride = xres * 4;
		bytes = static_cast<size_t>(stride) * yres;
		break;
	case NDIlib_FourCC_video_type_NV12:
		stride = xres;
		bytes = static_cast<size_t>(stride) * yres * 3 / 2;
		break;
	default: // I420 / YV12
		stride = xres;
		bytes = static_cast<size_t>(stride) * yres * 3 / 2;
		break;
	}

	if (!recv->buffer)
		recv->buffer = static_cast<uint8_t *>(calloc(1, static_cast<size_t>(xres) * yres * 4));

	video->xres = xres;
	video->yres = yres;
	video->FourCC = fourcc;
	video->frame_rate_N = 60000;
	video->frame_rate_D = 1001;
	video->picture_aspect_ratio = 16.0f / 9.0f;
	video->frame_format_type = NDIlib_frame_format_type_progressive;
	video->timecode = 0;
	video->timestamp = 1000000LL * (step + 1);
	video->p_data = recv->buffer;
	video->line_stride_in_bytes = stride;
	video->p_metadata = nullptr;

	(void)bytes;
	return NDIlib_frame_type_video;
}

void fake_recv_free_video_v2(NDIlib_recv_instance_t, const NDIlib_video_frame_v2_t *) {}
void fake_recv_free_audio_v3(NDIlib_recv_instance_t, const NDIlib_audio_frame_v3_t *) {}
void fake_recv_free_metadata(NDIlib_recv_instance_t, const NDIlib_metadata_frame_t *) {}

bool fake_recv_set_tally(NDIlib_recv_instance_t instance, const NDIlib_tally_t *tally)
{
	auto *recv = reinterpret_cast<FakeRecv *>(instance);
	if (tally)
		recv->tally = *tally;
	return true;
}

void fake_recv_get_performance(NDIlib_recv_instance_t instance, NDIlib_recv_performance_t *total,
			       NDIlib_recv_performance_t *dropped)
{
	auto *recv = reinterpret_cast<FakeRecv *>(instance);
	if (total)
		total->video_frames = recv->capture_count;
	if (dropped)
		dropped->video_frames = 3;
}

int fake_recv_get_no_connections(NDIlib_recv_instance_t)
{
	return 1;
}

NDIlib_v6 g_lib;

} // namespace

extern "C" const NDIlib_v6 *NDIlib_v6_load(void)
{
	memset(&g_lib, 0, sizeof(g_lib));

	g_lib.initialize = fake_initialize;
	g_lib.destroy = fake_destroy;
	g_lib.version = fake_version;
	g_lib.is_supported_CPU = fake_is_supported_CPU;

	g_lib.find_create_v2 = fake_find_create_v2;
	g_lib.find_destroy = fake_find_destroy;
	g_lib.find_wait_for_sources = fake_find_wait_for_sources;
	g_lib.find_get_current_sources = fake_find_get_current_sources;

	g_lib.recv_create_v3 = fake_recv_create_v3;
	g_lib.recv_destroy = fake_recv_destroy;
	g_lib.recv_capture_v3 = fake_recv_capture_v3;
	g_lib.recv_free_video_v2 = fake_recv_free_video_v2;
	g_lib.recv_free_audio_v3 = fake_recv_free_audio_v3;
	g_lib.recv_free_metadata = fake_recv_free_metadata;
	g_lib.recv_set_tally = fake_recv_set_tally;
	g_lib.recv_get_performance = fake_recv_get_performance;
	g_lib.recv_get_no_connections = fake_recv_get_no_connections;

	return &g_lib;
}
