/*
 * ZZ9000 SDK media-session shared-buffer output (ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz9000_sdk_media_output.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace zz9000_sdk {
namespace {

constexpr uint32_t weight_one = 4096;
// 4x4 Bayer thresholds, 0..15.
constexpr uint8_t bayer4[4][4] = {
	{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// Levels of the fixed palette's RGB cube by depth (index 0 unused). Depths 1
// and 2 are grey ramps.
constexpr uint32_t cube_levels[9][3] = {
	{0, 0, 0}, {2, 0, 0}, {4, 0, 0}, {2, 2, 2}, {2, 3, 2},
	{3, 3, 3}, {4, 4, 4}, {5, 5, 5}, {6, 7, 6}};

inline uint8_t clamp_u8(int32_t value)
{
	return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// Quantize one channel to `levels` steps. threshold is a Bayer value 0..15,
// or -1 to round to nearest.
inline uint32_t quantize(int32_t value, uint32_t levels, int threshold)
{
	value = std::clamp(value, 0, 255);
	const uint32_t steps = levels - 1;
	uint32_t q;
	if (threshold < 0)
		q = (static_cast<uint32_t>(value) * steps + 127) / 255;
	else
		q = (static_cast<uint32_t>(value) * steps * 32 +
		     static_cast<uint32_t>(2 * threshold + 1) * 255) / (255 * 32);
	return std::min(q, steps);
}

inline int32_t level_value(uint32_t level, uint32_t levels)
{
	return static_cast<int32_t>((level * 255 + (levels - 1) / 2) / (levels - 1));
}

inline void yuv_to_rgb(int32_t y, int32_t u, int32_t v, uint8_t *rgb)
{
	const int32_t c = 298 * (y - 16), d = u - 128, e = v - 128;
	rgb[0] = clamp_u8((c + 409 * e + 128) >> 8);
	rgb[1] = clamp_u8((c - 100 * d - 208 * e + 128) >> 8);
	rgb[2] = clamp_u8((c + 516 * d + 128) >> 8);
}

inline int32_t channel(uint32_t colour, int shift)
{
	return static_cast<int32_t>((colour >> shift) & 255);
}

} // namespace

uint32_t media_output_depth(const MediaOutputConfig &config)
{
	if (config.format != output_format_index8 && config.format != output_format_planar)
		return 0;
	const uint32_t depth = config.flags & output_depth_mask;
	return depth == 0 ? 8 : depth;
}

bool media_output_indexed(const MediaOutputConfig &config)
{
	return media_output_depth(config) != 0;
}

uint64_t media_output_picture_bytes(const MediaOutputConfig &config)
{
	if (!config.width || !config.height || config.width > 1920 || config.height > 1920 ||
	    static_cast<uint64_t>(config.width) * config.height > 1920u * 1088u)
		return 0;
	if (config.flags & ~output_known_flags)
		return 0;
	if ((config.flags & output_dither_ordered) && (config.flags & output_dither_diffusion))
		return 0;
	const uint64_t rows = config.height;
	switch (config.format) {
	case output_format_index8:
		if (config.pitch < config.width || (config.flags & output_depth_mask) > 8 ||
		    (config.flags & output_planar_interleaved))
			return 0;
		return rows * config.pitch;
	case output_format_planar: {
		const uint32_t depth = media_output_depth(config);
		if (depth > 8 || config.pitch < (config.width + 7) / 8)
			return 0;
		return rows * config.pitch * depth;
	}
	case output_format_rgb565:
	case output_format_rgb555:
		if (config.pitch < config.width * 2 || (config.pitch & 1) ||
		    (config.flags & (output_depth_mask | output_planar_interleaved |
		                     output_palette_host)))
			return 0;
		return rows * config.pitch;
	default:
		return 0;
	}
}

void media_output_fixed_palette(uint32_t depth, uint32_t palette[256])
{
	std::fill(palette, palette + 256, 0u);
	if (depth < 1 || depth > 8)
		return;
	if (depth <= 2) {
		const uint32_t levels = cube_levels[depth][0];
		for (uint32_t i = 0; i < levels; ++i) {
			const auto v = static_cast<uint32_t>(level_value(i, levels));
			palette[i] = (v << 16) | (v << 8) | v;
		}
		return;
	}
	const uint32_t lr = cube_levels[depth][0], lg = cube_levels[depth][1];
	const uint32_t lb = cube_levels[depth][2];
	for (uint32_t r = 0; r < lr; ++r)
		for (uint32_t g = 0; g < lg; ++g)
			for (uint32_t b = 0; b < lb; ++b)
				palette[(r * lg + g) * lb + b] =
					(static_cast<uint32_t>(level_value(r, lr)) << 16) |
					(static_cast<uint32_t>(level_value(g, lg)) << 8) |
					static_cast<uint32_t>(level_value(b, lb));
}

void MediaOutputConverter::configure(const MediaOutputConfig &config,
                                     const uint32_t *palette)
{
	config_ = config;
	depth_ = media_output_depth(config);
	grey_ = false;
	inverse_.clear();
	host_spread_ = 0;
	scaled_src_w_ = scaled_src_h_ = 0;
	if (depth_) {
		if ((config.flags & output_palette_host) && palette) {
			const uint32_t pens = 1u << depth_;
			std::copy(palette, palette + 256, palette_);
			for (uint32_t i = pens; i < 256; ++i)
				palette_[i] = 0;
			// Nearest pen for every RGB555 colour, so a pixel costs one lookup.
			inverse_.resize(32768);
			for (uint32_t c = 0; c < 32768; ++c) {
				const int32_t r = static_cast<int32_t>(((c >> 10) & 31) * 255 / 31);
				const int32_t g = static_cast<int32_t>(((c >> 5) & 31) * 255 / 31);
				const int32_t b = static_cast<int32_t>((c & 31) * 255 / 31);
				uint32_t best = 0;
				int32_t best_distance = INT32_MAX;
				for (uint32_t pen = 0; pen < pens; ++pen) {
					const int32_t dr = r - channel(palette_[pen], 16);
					const int32_t dg = g - channel(palette_[pen], 8);
					const int32_t db = b - channel(palette_[pen], 0);
					// Weighted towards green, as the eye is.
					const int32_t distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
					if (distance < best_distance) {
						best_distance = distance;
						best = pen;
					}
				}
				inverse_[c] = static_cast<uint8_t>(best);
			}
			const long side = std::max(1L, std::lround(std::cbrt(static_cast<double>(pens))));
			host_spread_ = std::min<int32_t>(128, static_cast<int32_t>(256 / side));
		} else {
			media_output_fixed_palette(depth_, palette_);
			grey_ = depth_ <= 2;
			std::copy(cube_levels[depth_], cube_levels[depth_] + 3, levels_);
		}
	} else {
		levels_[0] = 32;
		levels_[1] = config.format == output_format_rgb565 ? 64 : 32;
		levels_[2] = 32;
	}
	indices_.assign(config.width, 0);
	for (auto &row : error_)
		row.assign(static_cast<size_t>(config.width + 2) * 3, 0);
}

// One axis of a separable filter: a triangle (tent) filter whose support
// widens with the shrink ratio, so shrinking averages the area each output
// pixel covers and enlarging is bilinear. Edges are clamped.
void MediaOutputConverter::build_axis(uint32_t src, uint32_t dst, std::vector<Tap> &taps,
                                      std::vector<uint16_t> &weights) const
{
	taps.assign(dst, {});
	weights.clear();
	const double ratio = static_cast<double>(src) / dst;
	const bool nearest = (config_.flags & output_scale_nearest) != 0;
	const double support = std::max(1.0, ratio);
	std::vector<double> w;
	for (uint32_t i = 0; i < dst; ++i) {
		auto &tap = taps[i];
		tap.weights = static_cast<uint32_t>(weights.size());
		if (nearest) {
			tap.source = std::min<uint32_t>(src - 1, static_cast<uint32_t>((i + 0.5) * ratio));
			tap.count = 1;
			weights.push_back(static_cast<uint16_t>(weight_one));
			continue;
		}
		const double centre = (i + 0.5) * ratio - 0.5;
		const auto lo = static_cast<int64_t>(std::floor(centre - support)) + 1;
		const auto hi = static_cast<int64_t>(std::ceil(centre + support)) - 1;
		const int64_t first = std::clamp<int64_t>(lo, 0, src - 1);
		const int64_t last = std::clamp<int64_t>(hi, first, src - 1);
		w.assign(static_cast<size_t>(last - first + 1), 0.0);
		double total = 0;
		for (int64_t j = lo; j <= hi; ++j) {
			const double weight = std::max(0.0, 1.0 - std::fabs(j - centre) / support);
			w[static_cast<size_t>(std::clamp(j, first, last) - first)] += weight;
			total += weight;
		}
		if (total <= 0) {
			std::fill(w.begin(), w.end(), 0.0);
			w[static_cast<size_t>(std::clamp<int64_t>(std::lround(centre), first, last) - first)] = 1.0;
			total = 1.0;
		}
		int32_t sum = 0;
		size_t largest = 0;
		for (size_t k = 0; k < w.size(); ++k) {
			const auto value = static_cast<uint16_t>(std::lround(w[k] / total * weight_one));
			weights.push_back(value);
			sum += value;
			if (value > weights[tap.weights + largest])
				largest = k;
		}
		// Make the weights sum to exactly one.
		weights[tap.weights + largest] = static_cast<uint16_t>(
			weights[tap.weights + largest] + static_cast<int32_t>(weight_one) - sum);
		tap.source = static_cast<uint32_t>(first);
		tap.count = static_cast<uint32_t>(w.size());
	}
}

void MediaOutputConverter::scale(const uint8_t *rgb, uint32_t src_width, uint32_t src_height)
{
	const uint32_t width = config_.width, height = config_.height;
	if (src_width != scaled_src_w_ || src_height != scaled_src_h_) {
		build_axis(src_width, width, taps_x_, weights_x_);
		build_axis(src_height, height, taps_y_, weights_y_);
		scaled_src_w_ = src_width;
		scaled_src_h_ = src_height;
	}
	scaled_.resize(static_cast<size_t>(width) * height * 3);
	row_accum_.resize(static_cast<size_t>(src_width) * 3);
	for (uint32_t y = 0; y < height; ++y) {
		const auto &ty = taps_y_[y];
		std::fill(row_accum_.begin(), row_accum_.end(), 0u);
		for (uint32_t k = 0; k < ty.count; ++k) {
			const uint32_t weight = weights_y_[ty.weights + k];
			if (!weight)
				continue;
			const uint8_t *src = rgb + static_cast<size_t>(ty.source + k) * src_width * 3;
			for (size_t i = 0; i < row_accum_.size(); ++i)
				row_accum_[i] += src[i] * weight;
		}
		uint8_t *dst = scaled_.data() + static_cast<size_t>(y) * width * 3;
		for (uint32_t x = 0; x < width; ++x) {
			const auto &tx = taps_x_[x];
			uint64_t r = 0, g = 0, b = 0;
			for (uint32_t k = 0; k < tx.count; ++k) {
				const uint64_t weight = weights_x_[tx.weights + k];
				const uint32_t *s = row_accum_.data() + static_cast<size_t>(tx.source + k) * 3;
				r += s[0] * weight;
				g += s[1] * weight;
				b += s[2] * weight;
			}
			constexpr uint64_t round = (static_cast<uint64_t>(weight_one) * weight_one) / 2;
			constexpr uint64_t one = static_cast<uint64_t>(weight_one) * weight_one;
			dst[x * 3] = static_cast<uint8_t>(std::min<uint64_t>(255, (r + round) / one));
			dst[x * 3 + 1] = static_cast<uint8_t>(std::min<uint64_t>(255, (g + round) / one));
			dst[x * 3 + 2] = static_cast<uint8_t>(std::min<uint64_t>(255, (b + round) / one));
		}
	}
}

uint8_t MediaOutputConverter::map_index(int32_t r, int32_t g, int32_t b,
                                        uint32_t x, uint32_t y) const
{
	const int threshold = (config_.flags & output_dither_ordered) ?
		bayer4[y & 3][x & 3] : -1;
	if (!inverse_.empty()) {
		if (threshold >= 0) {
			const int32_t offset = ((2 * threshold + 1) - 16) * host_spread_ / 32;
			r += offset; g += offset; b += offset;
		}
		const uint32_t c = (static_cast<uint32_t>(clamp_u8(r)) >> 3 << 10) |
			(static_cast<uint32_t>(clamp_u8(g)) >> 3 << 5) |
			(static_cast<uint32_t>(clamp_u8(b)) >> 3);
		return inverse_[c];
	}
	if (grey_) {
		const int32_t luma = (77 * std::clamp(r, 0, 255) + 150 * std::clamp(g, 0, 255) +
			29 * std::clamp(b, 0, 255) + 128) >> 8;
		return static_cast<uint8_t>(quantize(luma, levels_[0], threshold));
	}
	const uint32_t qr = quantize(r, levels_[0], threshold);
	const uint32_t qg = quantize(g, levels_[1], threshold);
	const uint32_t qb = quantize(b, levels_[2], threshold);
	return static_cast<uint8_t>((qr * levels_[1] + qg) * levels_[2] + qb);
}

void MediaOutputConverter::quantize_row(uint32_t y, uint8_t *indices)
{
	const uint8_t *src = scaled_.data() + static_cast<size_t>(y) * config_.width * 3;
	if (!(config_.flags & output_dither_diffusion)) {
		for (uint32_t x = 0; x < config_.width; ++x)
			indices[x] = map_index(src[x * 3], src[x * 3 + 1], src[x * 3 + 2], x, y);
		return;
	}
	// Floyd-Steinberg. Errors are kept in sixteenths, offset by one pixel so
	// x - 1 never underflows.
	auto &cur = error_[y & 1];
	auto &next = error_[(y + 1) & 1];
	std::fill(next.begin(), next.end(), 0);
	for (uint32_t x = 0; x < config_.width; ++x) {
		int32_t want[3];
		for (int c = 0; c < 3; ++c)
			want[c] = std::clamp(src[x * 3 + c] + cur[(x + 1) * 3 + c] / 16, 0, 255);
		const uint8_t index = map_index(want[0], want[1], want[2], x, y);
		indices[x] = index;
		const int32_t got[3] = {
			channel(palette_[index], 16), channel(palette_[index], 8), channel(palette_[index], 0)};
		for (int c = 0; c < 3; ++c) {
			const int32_t error = want[c] - got[c];
			cur[(x + 2) * 3 + c] += error * 7;
			next[x * 3 + c] += error * 3;
			next[(x + 1) * 3 + c] += error * 5;
			next[(x + 2) * 3 + c] += error;
		}
	}
}

void MediaOutputConverter::write_rgb_row(uint32_t y, uint8_t *row)
{
	const uint8_t *src = scaled_.data() + static_cast<size_t>(y) * config_.width * 3;
	const bool r565 = config_.format == output_format_rgb565;
	const bool diffusion = (config_.flags & output_dither_diffusion) != 0;
	auto &cur = error_[y & 1];
	auto &next = error_[(y + 1) & 1];
	if (diffusion)
		std::fill(next.begin(), next.end(), 0);
	for (uint32_t x = 0; x < config_.width; ++x) {
		const int threshold = (config_.flags & output_dither_ordered) ?
			bayer4[y & 3][x & 3] : -1;
		uint32_t q[3];
		for (int c = 0; c < 3; ++c) {
			int32_t want = src[x * 3 + c];
			if (diffusion)
				want = std::clamp(want + cur[(x + 1) * 3 + c] / 16, 0, 255);
			q[c] = quantize(want, levels_[c], threshold);
			if (diffusion) {
				const int32_t error = want - level_value(q[c], levels_[c]);
				cur[(x + 2) * 3 + c] += error * 7;
				next[x * 3 + c] += error * 3;
				next[(x + 1) * 3 + c] += error * 5;
				next[(x + 2) * 3 + c] += error;
			}
		}
		const uint16_t value = r565 ?
			static_cast<uint16_t>((q[0] << 11) | (q[1] << 5) | q[2]) :
			static_cast<uint16_t>((q[0] << 10) | (q[1] << 5) | q[2]);
		row[x * 2] = static_cast<uint8_t>(value >> 8);
		row[x * 2 + 1] = static_cast<uint8_t>(value);
	}
}

void MediaOutputConverter::convert(const uint8_t *rgb, uint32_t src_width,
                                   uint32_t src_height, uint8_t *dst)
{
	if (!src_width || !src_height)
		return;
	scale(rgb, src_width, src_height);
	for (auto &row : error_)
		std::fill(row.begin(), row.end(), 0);
	const uint32_t width = config_.width, height = config_.height, pitch = config_.pitch;
	for (uint32_t y = 0; y < height; ++y) {
		if (!depth_) {
			uint8_t *row = dst + static_cast<size_t>(y) * pitch;
			std::memset(row + width * 2, 0, pitch - width * 2);
			write_rgb_row(y, row);
			continue;
		}
		quantize_row(y, indices_.data());
		if (config_.format == output_format_index8) {
			uint8_t *row = dst + static_cast<size_t>(y) * pitch;
			std::memcpy(row, indices_.data(), width);
			std::memset(row + width, 0, pitch - width);
			continue;
		}
		const bool interleaved = (config_.flags & output_planar_interleaved) != 0;
		for (uint32_t plane = 0; plane < depth_; ++plane) {
			uint8_t *row = dst + (interleaved ?
				(static_cast<size_t>(y) * depth_ + plane) * pitch :
				(static_cast<size_t>(plane) * height + y) * pitch);
			std::memset(row, 0, pitch);
			for (uint32_t x = 0; x < width; ++x)
				if ((indices_[x] >> plane) & 1)
					row[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
		}
	}
}

void media_yuy2_to_rgb(const uint8_t *yuy2, uint32_t width, uint32_t height,
                       bool interlaced, std::vector<uint8_t> &rgb)
{
	rgb.resize(static_cast<size_t>(width) * height * 3);
	const size_t stride = static_cast<size_t>(width) * 2;
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *row = yuy2 + y * stride;
		// Interlaced pictures are line-blended, as the overlay path does.
		const uint8_t *next = interlaced && y + 1 < height ? row + stride : row;
		uint8_t *out = rgb.data() + static_cast<size_t>(y) * width * 3;
		for (uint32_t x = 0; x + 1 < width + 1; x += 2) {
			const uint8_t *p = row + static_cast<size_t>(x) * 2;
			const uint8_t *q = next + static_cast<size_t>(x) * 2;
			const int32_t y0 = (p[0] + q[0] + 1) >> 1, u = (p[1] + q[1] + 1) >> 1;
			const int32_t y1 = (p[2] + q[2] + 1) >> 1, v = (p[3] + q[3] + 1) >> 1;
			yuv_to_rgb(y0, u, v, out + x * 3);
			if (x + 1 < width)
				yuv_to_rgb(y1, u, v, out + (x + 1) * 3);
		}
	}
}

void media_yuv420_to_rgb(const uint8_t *y_plane, uint32_t y_stride,
                         const uint8_t *cb, const uint8_t *cr, uint32_t c_stride,
                         uint32_t width, uint32_t height, std::vector<uint8_t> &rgb)
{
	rgb.resize(static_cast<size_t>(width) * height * 3);
	for (uint32_t y = 0; y < height; ++y) {
		uint8_t *out = rgb.data() + static_cast<size_t>(y) * width * 3;
		const uint8_t *luma = y_plane + static_cast<size_t>(y) * y_stride;
		const uint8_t *u = cb + static_cast<size_t>(y / 2) * c_stride;
		const uint8_t *v = cr + static_cast<size_t>(y / 2) * c_stride;
		for (uint32_t x = 0; x < width; ++x)
			yuv_to_rgb(luma[x], u[x / 2], v[x / 2], out + x * 3);
	}
}

} // namespace zz9000_sdk
