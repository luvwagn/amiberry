/*
 * ZZ9000 SDK media-session shared-buffer output: scaling and pixel-format
 * conversion of a decoded picture into a host-visible buffer
 * (ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AMIBERRY_ZZ9000_SDK_MEDIA_OUTPUT_H
#define AMIBERRY_ZZ9000_SDK_MEDIA_OUTPUT_H

#include <cstdint>
#include <vector>

namespace zz9000_sdk {

// ZZ9K_VIDEO_OUTPUT_* values carried by MEDIA_SESSION_BEGIN.
constexpr uint32_t video_output_direct_overlay = 1;
constexpr uint32_t video_output_shared_buffer = 2;

// ZZ9K_SURFACE_FORMAT_* values the shared-buffer output accepts.
constexpr uint32_t output_format_rgb565 = 1;
constexpr uint32_t output_format_index8 = 4;
constexpr uint32_t output_format_planar = 5;
constexpr uint32_t output_format_rgb555 = 6;

// ZZ9KMediaOutputFlags.
constexpr uint32_t output_depth_mask = 0x0f;
constexpr uint32_t output_planar_interleaved = 1u << 4;
constexpr uint32_t output_dither_ordered = 1u << 5;
constexpr uint32_t output_dither_diffusion = 1u << 6;
constexpr uint32_t output_palette_host = 1u << 7;
constexpr uint32_t output_scale_nearest = 1u << 8;
constexpr uint32_t output_known_flags = 0x1ff;

struct MediaOutputConfig {
	uint32_t width = 0, height = 0;
	uint32_t format = 0;
	uint32_t pitch = 0;
	uint32_t flags = 0;
};

// Bits per pixel of an indexed output (1..8), or 0 for RGB formats.
uint32_t media_output_depth(const MediaOutputConfig &config);
bool media_output_indexed(const MediaOutputConfig &config);
// Bytes one picture occupies, or 0 when the configuration is invalid.
uint64_t media_output_picture_bytes(const MediaOutputConfig &config);
// The card's fixed palette for a depth: an RGB cube (greys for depth 1-2),
// index = (r * G + g) * B + b, unused entries 0. 256 0x00RRGGBB words.
void media_output_fixed_palette(uint32_t depth, uint32_t palette[256]);

class MediaOutputConverter {
public:
	// palette: 256 0x00RRGGBB words read from the host when PALETTE_HOST is
	// set; ignored otherwise.
	void configure(const MediaOutputConfig &config, const uint32_t *palette);
	const MediaOutputConfig &config() const { return config_; }
	// rgb is src_width * src_height packed R,G,B bytes. Writes one picture
	// of media_output_picture_bytes() at dst.
	void convert(const uint8_t *rgb, uint32_t src_width, uint32_t src_height,
	             uint8_t *dst);

private:
	struct Tap { uint32_t source = 0, weights = 0, count = 0; };
	void build_axis(uint32_t src, uint32_t dst, std::vector<Tap> &taps,
	                std::vector<uint16_t> &weights) const;
	void scale(const uint8_t *rgb, uint32_t src_width, uint32_t src_height);
	uint8_t map_index(int32_t r, int32_t g, int32_t b, uint32_t x, uint32_t y) const;
	void quantize_row(uint32_t y, uint8_t *indices);
	void write_rgb_row(uint32_t y, uint8_t *row);

	MediaOutputConfig config_{};
	uint32_t depth_ = 0;
	uint32_t levels_[3] = {};
	bool grey_ = false;
	uint32_t palette_[256] = {};
	std::vector<uint8_t> inverse_; // RGB555 -> nearest host pen
	int32_t host_spread_ = 0;      // ordered-dither amplitude for host palettes
	// Scaled picture, 3 bytes per pixel at the output size.
	std::vector<uint8_t> scaled_;
	uint32_t scaled_src_w_ = 0, scaled_src_h_ = 0;
	std::vector<Tap> taps_x_, taps_y_;
	std::vector<uint16_t> weights_x_, weights_y_;
	std::vector<uint32_t> row_accum_;
	// Floyd-Steinberg error rows (current and next), 3 ints per pixel.
	std::vector<int32_t> error_[2];
	std::vector<uint8_t> indices_;
};

// YUV to packed RGB at the source size (BT.601 limited range, the matrix the
// card's overlay plane applies).
void media_yuy2_to_rgb(const uint8_t *yuy2, uint32_t width, uint32_t height,
                       bool interlaced, std::vector<uint8_t> &rgb);
void media_yuv420_to_rgb(const uint8_t *y, uint32_t y_stride,
                         const uint8_t *cb, const uint8_t *cr, uint32_t c_stride,
                         uint32_t width, uint32_t height, std::vector<uint8_t> &rgb);

} // namespace zz9000_sdk

#endif
