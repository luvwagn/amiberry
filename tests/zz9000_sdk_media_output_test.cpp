// ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER: media-session pictures scaled and
// converted into a host-visible buffer (zz9000-firmware issue #238).
#include "zz9000_sdk.h"
#include "zz9000_sdk_media_output.h"
#include "zz9000_sdk_test_mailbox.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <vector>

using namespace zz9000_sdk;

namespace {
uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
uint32_t get32(const uint8_t *p) { return (get16(p) << 16) | get16(p + 2); }
uint64_t get64(const uint8_t *p)
{
	return (static_cast<uint64_t>(get32(p)) << 32) | get32(p + 4);
}
void put16(uint8_t *p, uint16_t n) { p[0] = n >> 8; p[1] = n; }
void put32(uint8_t *p, uint32_t n) { put16(p, n >> 16); put16(p + 2, n); }

constexpr uint16_t status_ok = 0, status_busy = 2, status_unsupported = 3;
constexpr uint16_t status_bad_request = 4, status_bad_handle = 5;
constexpr uint32_t flag_presented = 1u << 9, flag_slot1 = 1u << 15;
constexpr uint32_t flag_pending = 1u << 16;

struct Mailbox {
	std::vector<uint8_t> board;
	Engine sdk;
	uint8_t *mb;
	uint32_t entries;
	uint32_t request_id = 0, completion_head = 0;
	uint16_t last_status = 0;

	Mailbox()
		: board(128 * 1024 * 1024), sdk(board.data(), board.size()),
		  mb(zz9000_test_mailbox(sdk, board.data())), entries(get32(mb + 16)) {}

	const uint8_t *call(uint16_t opcode, const uint8_t *payload,
	                    uint16_t length, uint16_t status = 0) {
		const uint32_t tail = get32(mb + 24);
		auto *request = mb + get32(mb + 12) + tail * 64;
		std::fill(request, request + 64, 0);
		put32(request, ++request_id);
		put16(request + 4, opcode);
		put16(request + 10, length);
		if (payload)
			std::copy(payload, payload + length, request + 16);
		put32(mb + 24, (tail + 1) % entries);
		sdk.poll();
		const auto *reply = mb + get32(mb + 28) + completion_head * 64;
		assert(get32(reply) == request_id);
		last_status = get16(reply + 6);
		if (status != 0xffff && last_status != status) {
			std::fprintf(stderr, "opcode %04x: status %u, expected %u\n",
				opcode, last_status, status);
			std::abort();
		}
		completion_head = (completion_head + 1) % entries;
		put32(mb + 36, completion_head);
		return reply + 16;
	}

	uint32_t allocate(uint32_t length, uint32_t flags = 1) {
		uint8_t request[48] = {};
		put32(request, length);
		put32(request + 4, 16);
		put32(request + 8, flags);
		return get32(call(0x0100, request, 12));
	}
};

// ---------------------------------------------------------------------------
// Converter unit checks

std::vector<uint8_t> solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
{
	std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
	for (size_t i = 0; i < rgb.size(); i += 3) {
		rgb[i] = r; rgb[i + 1] = g; rgb[i + 2] = b;
	}
	return rgb;
}

// Pens of a planar picture, read back plane by plane.
std::vector<uint8_t> planar_pens(const uint8_t *picture, const MediaOutputConfig &config)
{
	const uint32_t depth = media_output_depth(config);
	const bool interleaved = (config.flags & output_planar_interleaved) != 0;
	std::vector<uint8_t> pens(static_cast<size_t>(config.width) * config.height);
	for (uint32_t y = 0; y < config.height; ++y)
		for (uint32_t plane = 0; plane < depth; ++plane) {
			const uint8_t *row = picture + (interleaved ?
				(static_cast<size_t>(y) * depth + plane) * config.pitch :
				(static_cast<size_t>(plane) * config.height + y) * config.pitch);
			for (uint32_t x = 0; x < config.width; ++x)
				if (row[x >> 3] & (0x80 >> (x & 7)))
					pens[static_cast<size_t>(y) * config.width + x] |= 1u << plane;
		}
	return pens;
}

void test_validation()
{
	MediaOutputConfig c;
	c.width = 320; c.height = 240; c.format = output_format_planar; c.pitch = 40;
	assert(media_output_picture_bytes(c) == 320u * 240 / 8 * 8);
	c.flags = 5;
	assert(media_output_picture_bytes(c) == 40u * 240 * 5);
	c.pitch = 39;
	assert(media_output_picture_bytes(c) == 0); // a row needs 40 bytes
	c.pitch = 40; c.flags = 9;
	assert(media_output_picture_bytes(c) == 0); // depth above 8
	c.flags = output_dither_ordered | output_dither_diffusion;
	assert(media_output_picture_bytes(c) == 0);
	c.flags = 1u << 9;
	assert(media_output_picture_bytes(c) == 0); // unknown flag
	c.flags = 0; c.format = output_format_index8; c.pitch = 320;
	assert(media_output_picture_bytes(c) == 320u * 240);
	c.flags = output_planar_interleaved;
	assert(media_output_picture_bytes(c) == 0);
	c.flags = 0; c.format = output_format_rgb565; c.pitch = 640;
	assert(media_output_picture_bytes(c) == 640u * 240);
	c.pitch = 641;
	assert(media_output_picture_bytes(c) == 0); // odd pitch
	c.pitch = 640; c.flags = output_palette_host;
	assert(media_output_picture_bytes(c) == 0); // RGB has no palette
	c.flags = 0; c.format = 2; // ARGB8888 is not an output format
	assert(media_output_picture_bytes(c) == 0);
	c.format = output_format_rgb555; c.width = 0;
	assert(media_output_picture_bytes(c) == 0);
	c.width = 1920; c.height = 1200; c.pitch = 3840;
	assert(media_output_picture_bytes(c) == 0); // above 1920 x 1088 pixels
}

void test_fixed_palette()
{
	uint32_t palette[256];
	media_output_fixed_palette(8, palette);
	assert(palette[0] == 0 && palette[251] == 0x00ffffffu && palette[252] == 0);
	assert(palette[(5 * 7 + 0) * 6 + 0] == 0x00ff0000u); // pure red
	media_output_fixed_palette(5, palette);
	assert(palette[26] == 0x00ffffffu && palette[27] == 0);
	assert(palette[(1 * 3 + 1) * 3 + 1] == 0x00808080u);
	media_output_fixed_palette(1, palette);
	assert(palette[0] == 0 && palette[1] == 0x00ffffffu && palette[2] == 0);
	media_output_fixed_palette(2, palette);
	assert(palette[1] == 0x00555555u && palette[2] == 0x00aaaaaau);
}

void test_solid_colours()
{
	MediaOutputConverter converter;
	MediaOutputConfig c;
	c.width = 20; c.height = 4; c.format = output_format_index8; c.pitch = 24;
	converter.configure(c, nullptr);
	std::vector<uint8_t> out(c.pitch * c.height, 0xee);
	auto red = solid(40, 8, 255, 0, 0);
	converter.convert(red.data(), 40, 8, out.data());
	for (uint32_t y = 0; y < c.height; ++y) {
		for (uint32_t x = 0; x < c.width; ++x)
			assert(out[y * c.pitch + x] == 210);
		for (uint32_t x = c.width; x < c.pitch; ++x)
			assert(out[y * c.pitch + x] == 0); // padding is cleared
	}

	c.format = output_format_rgb565; c.pitch = 40;
	converter.configure(c, nullptr);
	out.assign(c.pitch * c.height, 0xee);
	auto white = solid(7, 3, 255, 255, 255);
	converter.convert(white.data(), 7, 3, out.data());
	assert(out[0] == 0xff && out[1] == 0xff && out[38] == 0xff);
	c.format = output_format_rgb555;
	converter.configure(c, nullptr);
	converter.convert(white.data(), 7, 3, out.data());
	assert(out[0] == 0x7f && out[1] == 0xff);
	auto blue = solid(7, 3, 0, 0, 255);
	converter.convert(blue.data(), 7, 3, out.data());
	assert(out[0] == 0x00 && out[1] == 0x1f);
}

void test_planar_layouts()
{
	// A colourful picture, then the same pens from INDEX8 and from both
	// planar layouts.
	const uint32_t sw = 64, sh = 48;
	std::vector<uint8_t> rgb(sw * sh * 3);
	for (uint32_t y = 0; y < sh; ++y)
		for (uint32_t x = 0; x < sw; ++x) {
			auto *p = &rgb[(y * sw + x) * 3];
			p[0] = static_cast<uint8_t>(x * 4); p[1] = static_cast<uint8_t>(y * 5);
			p[2] = static_cast<uint8_t>((x + y) * 2);
		}
	for (uint32_t depth : {8u, 5u, 3u, 1u}) {
		MediaOutputConfig index;
		index.width = 37; index.height = 29; index.format = output_format_index8;
		index.pitch = 40; index.flags = (depth & 7) | output_dither_ordered;
		MediaOutputConverter converter;
		converter.configure(index, nullptr);
		std::vector<uint8_t> chunky(index.pitch * index.height);
		converter.convert(rgb.data(), sw, sh, chunky.data());
		std::set<uint8_t> used;
		for (uint32_t y = 0; y < index.height; ++y)
			for (uint32_t x = 0; x < index.width; ++x) {
				const uint8_t pen = chunky[y * index.pitch + x];
				assert(pen < (1u << depth));
				used.insert(pen);
			}
		assert(used.size() > 1);
		for (bool interleaved : {false, true}) {
			MediaOutputConfig planar = index;
			planar.format = output_format_planar;
			planar.pitch = 6; // 37 pixels need 5 bytes; one spare
			if (interleaved)
				planar.flags |= output_planar_interleaved;
			converter.configure(planar, nullptr);
			std::vector<uint8_t> picture(media_output_picture_bytes(planar), 0xee);
			converter.convert(rgb.data(), sw, sh, picture.data());
			const auto pens = planar_pens(picture.data(), planar);
			for (uint32_t y = 0; y < index.height; ++y)
				for (uint32_t x = 0; x < index.width; ++x)
					assert(pens[y * index.width + x] == chunky[y * index.pitch + x]);
			// Bits past the picture's width, and the spare byte, are zero.
			for (size_t row = 0; row < picture.size() / planar.pitch; ++row) {
				assert((picture[row * planar.pitch + 4] & 0x07) == 0);
				assert(picture[row * planar.pitch + 5] == 0);
			}
		}
	}
}

void test_scaling()
{
	// 1-pixel checkerboard: shrinking by 2 averages to grey, nearest keeps
	// one colour of the pattern.
	const uint32_t sw = 16, sh = 16;
	std::vector<uint8_t> rgb(sw * sh * 3);
	for (uint32_t y = 0; y < sh; ++y)
		for (uint32_t x = 0; x < sw; ++x)
			std::fill_n(&rgb[(y * sw + x) * 3], 3, ((x ^ y) & 1) ? 255 : 0);
	MediaOutputConfig c;
	c.width = 8; c.height = 8; c.format = output_format_rgb555; c.pitch = 16;
	MediaOutputConverter converter;
	converter.configure(c, nullptr);
	std::vector<uint8_t> out(c.pitch * c.height);
	converter.convert(rgb.data(), sw, sh, out.data());
	for (uint32_t i = 0; i < 64; ++i) {
		const uint16_t v = get16(&out[i * 2]);
		const uint32_t r = (v >> 10) & 31;
		assert(r >= 14 && r <= 17); // mid grey, not black or white
	}
	c.flags = output_scale_nearest;
	converter.configure(c, nullptr);
	converter.convert(rgb.data(), sw, sh, out.data());
	for (uint32_t i = 0; i < 64; ++i) {
		const uint16_t v = get16(&out[i * 2]);
		assert(v == 0 || v == 0x7fff);
	}
	// Enlarging a 2x1 black/white picture gives a ramp across the middle.
	std::vector<uint8_t> two = {0, 0, 0, 255, 255, 255};
	c.width = 8; c.height = 1; c.flags = 0;
	converter.configure(c, nullptr);
	converter.convert(two.data(), 2, 1, out.data());
	uint32_t previous = 0;
	for (uint32_t x = 0; x < 8; ++x) {
		const uint32_t r = (get16(&out[x * 2]) >> 10) & 31;
		assert(r >= previous);
		previous = r;
	}
	assert(((get16(&out[0]) >> 10) & 31) == 0 && ((get16(&out[14]) >> 10) & 31) == 31);
}

void test_dithering()
{
	// Mid grey to black and white: ordered dither and error diffusion both
	// light about half the pixels; without dither it is all one pen.
	MediaOutputConfig c;
	c.width = 32; c.height = 32; c.format = output_format_index8; c.pitch = 32;
	auto grey = solid(32, 32, 128, 128, 128);
	std::vector<uint8_t> out(32 * 32);
	for (uint32_t dither : {0u, output_dither_ordered, output_dither_diffusion}) {
		c.flags = 1 | dither;
		MediaOutputConverter converter;
		converter.configure(c, nullptr);
		converter.convert(grey.data(), 32, 32, out.data());
		const auto lit = std::count(out.begin(), out.end(), 1);
		if (!dither)
			assert(lit == 0 || lit == 1024);
		else
			assert(lit > 400 && lit < 624);
	}
}

void test_host_palette()
{
	// A host palette of 4 pens; each colour maps to the nearest one.
	uint32_t palette[256] = {};
	palette[0] = 0x000000; palette[1] = 0xff0000; palette[2] = 0x00ff00; palette[3] = 0xffffff;
	MediaOutputConfig c;
	c.width = 4; c.height = 1; c.format = output_format_index8; c.pitch = 4;
	c.flags = 2 | output_palette_host;
	MediaOutputConverter converter;
	converter.configure(c, palette);
	std::vector<uint8_t> rgb = {10, 10, 10, 230, 20, 20, 20, 220, 30, 240, 240, 240};
	std::vector<uint8_t> out(4);
	converter.convert(rgb.data(), 4, 1, out.data());
	assert(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3);
}

// ---------------------------------------------------------------------------
// Through the mailbox

void begin_request(uint8_t *request, uint32_t video_codec, uint32_t width,
                   uint32_t height, uint32_t output_format)
{
	std::fill(request, request + 48, 0);
	put32(request, video_codec); put32(request + 4, 1);
	put32(request + 8, width); put32(request + 12, height);
	put32(request + 16, output_format);
}

void output_request(uint8_t *request, uint32_t session, uint32_t handle,
                    uint32_t offset, uint32_t stride, uint32_t width,
                    uint32_t height, uint32_t format, uint32_t pitch,
                    uint32_t flags, uint32_t palette = 0, uint32_t palette_offset = 0)
{
	std::fill(request, request + 48, 0);
	put32(request, session); put32(request + 4, handle);
	put32(request + 8, offset); put32(request + 12, stride);
	put32(request + 16, width); put32(request + 20, height);
	put32(request + 24, format); put32(request + 28, pitch);
	put32(request + 32, flags); put32(request + 36, palette);
	put32(request + 40, palette_offset);
}

void test_discovery()
{
	Mailbox mailbox;
	uint8_t request[48] = {};
	put32(request, 0x0b00);
	const auto *reply = mailbox.call(0x0004, request, 4);
	assert(get32(reply + 20) == 15); // 0x0b00-0x0b0e
}

// Feed a whole file, decode every picture and present it into two slots.
// Returns the number of pictures presented.
uint32_t run_session(const char *path, uint32_t video_codec, uint32_t width,
                     uint32_t height)
{
	std::ifstream file(path, std::ios::binary);
	const std::vector<uint8_t> input{std::istreambuf_iterator<char>{file}, {}};
	assert(!input.empty());
	Mailbox mailbox;
	const uint32_t staging = mailbox.allocate(65536);
	// 320 x 240, 8 planes: 76,800 bytes a slot, two slots, a guard after.
	const uint32_t out_w = 320, out_h = 240, pitch = 40, depth = 8;
	const uint32_t picture = pitch * out_h * depth;
	const uint32_t output = mailbox.allocate(2 * picture + 64);
	const uint32_t palette = mailbox.allocate(1024);
	const uint32_t card_only = mailbox.allocate(2 * picture, 6);
	uint8_t *out = mailbox.sdk.buffer_data(output);
	std::fill(out, out + 2 * picture + 64, 0xa5);
	uint8_t request[48];

	// An overlay session cannot take an output configuration.
	begin_request(request, video_codec, width, height, video_output_direct_overlay);
	uint32_t session = get32(mailbox.call(0x0b04, request, 44));
	output_request(request, session, output, 0, picture, out_w, out_h,
		output_format_planar, pitch, depth & 7, palette);
	mailbox.call(0x0b0e, request, 48, status_bad_request);
	std::fill(request, request + 48, 0);
	put32(request, session);
	mailbox.call(0x0b0d, request, 16);
	// Unknown output formats are unsupported.
	begin_request(request, video_codec, width, height, 3);
	mailbox.call(0x0b04, request, 44, status_unsupported);

	// No overlay is configured at all: the shared-buffer session needs none.
	begin_request(request, video_codec, width, height, video_output_shared_buffer);
	session = get32(mailbox.call(0x0b04, request, 44));
	assert(session);
	// Configuration errors.
	output_request(request, session, card_only, 0, picture, out_w, out_h,
		output_format_planar, pitch, depth & 7);
	mailbox.call(0x0b0e, request, 48, status_bad_handle);
	output_request(request, session, output, 128, picture, out_w, out_h,
		output_format_planar, pitch, depth & 7);
	mailbox.call(0x0b0e, request, 48, status_bad_request); // slot 1 overflows
	output_request(request, session, output, 0, picture - 1, out_w, out_h,
		output_format_planar, pitch, depth & 7);
	mailbox.call(0x0b0e, request, 48, status_bad_request); // slots overlap
	output_request(request, session, output, 0, picture, out_w, out_h,
		output_format_planar, pitch, (depth & 7) | output_palette_host);
	mailbox.call(0x0b0e, request, 48, status_bad_request); // no palette given
	output_request(request, session, output, 0, picture, out_w, out_h,
		output_format_planar, pitch, depth & 7, palette);
	put32(request + 44, 1);
	mailbox.call(0x0b0e, request, 48, status_bad_request); // reserved
	put32(request + 44, 0);
	mailbox.call(0x0b0e, request, 47, status_bad_request); // short payload
	// Status page 6 before configuration.
	uint8_t status[48] = {};
	put32(status, session); put32(status + 4, 6);
	const auto *page = mailbox.call(0x0b0a, status, 16);
	assert(get32(page + 12) == 0);

	// The real configuration: the card writes its fixed palette.
	auto *pal = mailbox.sdk.buffer_data(palette);
	std::fill(pal, pal + 1024, 0xcc);
	const auto *reply = mailbox.call(0x0b0e, request, 48);
	assert(get32(reply) == session);
	assert(get32(pal + 251 * 4) == 0x00ffffffu && get32(pal) == 0 && get32(pal + 252 * 4) == 0);
	page = mailbox.call(0x0b0a, status, 16);
	assert(get32(page + 12) == 1); // configured, nothing written yet
	assert(get64(page + 32) == ((static_cast<uint64_t>(out_w) << 16) | out_h));
	assert(get64(page + 40) == ((static_cast<uint64_t>(output_format_planar) << 32) | pitch));

	size_t offset = 0;
	uint32_t presented = 0;
	bool done = false;
	for (unsigned round = 0; round < 4000 && !done; ++round) {
		while (offset < input.size()) {
			const uint32_t bytes = static_cast<uint32_t>(std::min<size_t>(4096, input.size() - offset));
			std::copy(input.begin() + offset, input.begin() + offset + bytes,
				mailbox.sdk.buffer_data(staging));
			std::fill(request, request + 48, 0);
			put32(request, session); put32(request + 4, staging); put32(request + 12, bytes);
			put32(request + 16, offset + bytes == input.size());
			reply = mailbox.call(0x0b05, request, 20, 0xffff);
			const uint32_t accepted = mailbox.last_status == status_ok ? get32(reply + 40) : 0;
			offset += accepted;
			if (accepted < bytes)
				break;
		}
		std::fill(request, request + 48, 0);
		put32(request, session);
		reply = mailbox.call(0x0b06, request, 16, 0xffff);
		if (mailbox.last_status == status_busy) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		assert(mailbox.last_status == status_ok);
		const uint32_t state = get32(reply + 4);
		if (state == 4) {
			done = true;
		} else if (state == 3) {
			const uint64_t pts = get64(reply + 28);
			reply = mailbox.call(0x0b08, request, 16);
			const uint32_t flags = get32(reply + 44);
			assert(flags & flag_presented);
			assert(!(flags & flag_pending)); // the emulator writes before replying
			const uint32_t slot = (flags & flag_slot1) ? 1 : 0;
			assert(slot == (presented & 1)); // slots alternate from slot 0
			++presented;
			page = mailbox.call(0x0b0a, status, 16);
			assert(get32(page + 12) == (1u | 2u | (slot ? 4u : 0u)));
			assert(get64(page + 16) == presented);
			assert(get64(page + 24) == pts);
			// The picture is a real one: many pens, every pen in range.
			const auto pens = planar_pens(out + slot * picture,
				MediaOutputConfig{out_w, out_h, output_format_planar, pitch, depth & 7});
			std::set<uint8_t> used(pens.begin(), pens.end());
			assert(used.size() > 16);
			for (uint8_t pen : used)
				assert(pen < 252);
			// PRESENT without a held picture is refused.
			mailbox.call(0x0b08, request, 16, status_bad_request);
		}
	}
	assert(done && presented >= 2);
	// Nothing was written past the second slot.
	for (uint32_t i = 0; i < 64; ++i)
		assert(out[2 * picture + i] == 0xa5);
	std::fill(request, request + 48, 0);
	put32(request, session);
	mailbox.call(0x0b0d, request, 16);
	return presented;
}

// RGB565 into a single slot, reconfigured to INDEX8 with a host palette
// between pictures; checks the later pictures go to slot 0.
void run_reconfigure(const char *path, uint32_t video_codec, uint32_t width, uint32_t height)
{
	std::ifstream file(path, std::ios::binary);
	const std::vector<uint8_t> input{std::istreambuf_iterator<char>{file}, {}};
	Mailbox mailbox;
	const uint32_t staging = mailbox.allocate(65536);
	const uint32_t output = mailbox.allocate(2 * width * height * 2);
	const uint32_t palette = mailbox.allocate(1024);
	uint8_t request[48];
	begin_request(request, video_codec, width, height, video_output_shared_buffer);
	const uint32_t session = get32(mailbox.call(0x0b04, request, 44));
	output_request(request, session, output, 0, width * height * 2, width, height,
		output_format_rgb565, width * 2, output_dither_ordered);
	mailbox.call(0x0b0e, request, 48);
	size_t offset = 0;
	uint32_t presented = 0;
	for (unsigned round = 0; round < 4000 && presented < 6; ++round) {
		while (offset < input.size()) {
			const uint32_t bytes = static_cast<uint32_t>(std::min<size_t>(4096, input.size() - offset));
			std::copy(input.begin() + offset, input.begin() + offset + bytes,
				mailbox.sdk.buffer_data(staging));
			std::fill(request, request + 48, 0);
			put32(request, session); put32(request + 4, staging); put32(request + 12, bytes);
			put32(request + 16, offset + bytes == input.size());
			const auto *reply = mailbox.call(0x0b05, request, 20, 0xffff);
			const uint32_t accepted = mailbox.last_status == status_ok ? get32(reply + 40) : 0;
			offset += accepted;
			if (accepted < bytes)
				break;
		}
		std::fill(request, request + 48, 0);
		put32(request, session);
		const auto *reply = mailbox.call(0x0b06, request, 16, 0xffff);
		if (mailbox.last_status == status_busy)
			continue;
		if (get32(reply + 4) == 4)
			break;
		if (get32(reply + 4) != 3)
			continue;
		reply = mailbox.call(0x0b08, request, 16);
		const uint32_t slot = (get32(reply + 44) & flag_slot1) ? 1 : 0;
		assert(slot == (presented < 3 ? presented & 1 : (presented - 3) & 1));
		++presented;
		if (presented == 3) {
			// Switch to 16 host pens in INDEX8 before the fourth picture.
			auto *pal = mailbox.sdk.buffer_data(palette);
			for (uint32_t i = 0; i < 16; ++i)
				put32(pal + i * 4, (i * 17) << 16 | (i * 17) << 8 | (255 - i * 17));
			output_request(request, session, output, 0, width * height, width, height,
				output_format_index8, width, 4 | output_palette_host, palette);
			mailbox.call(0x0b0e, request, 48);
			// The host's palette is left as it was.
			assert(get32(pal + 15 * 4) == ((255u << 16) | (255u << 8)));
		}
		if (presented == 4) {
			const uint8_t *picture = mailbox.sdk.buffer_data(output);
			for (uint32_t i = 0; i < width * height; ++i)
				assert(picture[i] < 16);
		}
	}
	assert(presented >= 5);
	std::fill(request, request + 48, 0);
	put32(request, session);
	mailbox.call(0x0b0d, request, 16);
}
} // namespace

int main(int argc, char **argv)
{
	test_validation();
	test_fixed_palette();
	test_solid_colours();
	test_planar_layouts();
	test_scaling();
	test_dithering();
	test_host_palette();
	test_discovery();
	// argv: an MPEG-1 file (pl_mpeg) and optionally an MPEG-2 file (FFmpeg),
	// both 160 x 120.
	if (argc >= 2) {
		const uint32_t pictures = run_session(argv[1], 1, 160, 120);
		std::printf("mpeg1: %u pictures presented to the shared buffer\n", pictures);
		run_reconfigure(argv[1], 1, 160, 120);
	}
#ifdef AMIBERRY_WITH_FFMPEG
	if (argc >= 3) {
		const uint32_t pictures = run_session(argv[2], 2, 160, 120);
		std::printf("mpeg2: %u pictures presented to the shared buffer\n", pictures);
		run_reconfigure(argv[2], 2, 160, 120);
	}
#endif
	std::printf("shared-buffer output: all tests passed\n");
	return 0;
}
