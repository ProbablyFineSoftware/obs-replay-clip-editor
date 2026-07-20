/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "VideoProbe.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

AVFormatContext *open_input(const QString &path)
{
	AVFormatContext *fmt = nullptr;
	QByteArray utf8 = path.toUtf8();
	if (avformat_open_input(&fmt, utf8.constData(), nullptr, nullptr) < 0)
		return nullptr;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return nullptr;
	}
	return fmt;
}

} // namespace

namespace VideoProbe {

ClipInfo probe(const QString &path)
{
	ClipInfo info;
	info.path = path;

	AVFormatContext *fmt = open_input(path);
	if (!fmt)
		return info;

	if (fmt->duration != AV_NOPTS_VALUE)
		info.durationMs = fmt->duration / (AV_TIME_BASE / 1000);

	int trackNumber = 1;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		AVStream *st = fmt->streams[i];
		if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && info.width == 0) {
			info.width = st->codecpar->width;
			info.height = st->codecpar->height;
			AVRational fr = st->avg_frame_rate;
			if (fr.num > 0 && fr.den > 0)
				info.fps = av_q2d(fr);
			if (info.durationMs == 0 && st->duration != AV_NOPTS_VALUE)
				info.durationMs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000});
		} else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			AudioTrackInfo track;
			track.streamIndex = (int)i;
			track.channels = st->codecpar->ch_layout.nb_channels;
			const AVCodecDescriptor *desc = avcodec_descriptor_get(st->codecpar->codec_id);
			track.codecName = desc ? QString::fromUtf8(desc->name) : QStringLiteral("?");

			AVDictionaryEntry *title = av_dict_get(st->metadata, "title", nullptr, 0);
			if (title && *title->value)
				track.title = QString::fromUtf8(title->value);
			else
				track.title = QStringLiteral("Track %1").arg(trackNumber);
			trackNumber++;
			info.audioTracks.push_back(track);
		}
	}

	info.valid = info.width > 0 && info.durationMs > 0;
	avformat_close_input(&fmt);
	return info;
}

QImage extractFrame(const QString &path, qint64 timeMs, int maxWidth)
{
	QImage result;
	AVFormatContext *fmt = open_input(path);
	if (!fmt)
		return result;

	int videoIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (videoIdx < 0) {
		avformat_close_input(&fmt);
		return result;
	}
	AVStream *st = fmt->streams[videoIdx];

	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (ctx)
		ctx->thread_count = 0; /* auto */
	if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, dec, nullptr) < 0) {
		if (ctx)
			avcodec_free_context(&ctx);
		avformat_close_input(&fmt);
		return result;
	}

	/* thumbnails don't need pristine frames — decode as fast as possible */
	ctx->skip_loop_filter = AVDISCARD_ALL;
	ctx->skip_frame = AVDISCARD_NONREF;

	int64_t target = av_rescale_q(timeMs, AVRational{1, 1000}, st->time_base);
	av_seek_frame(fmt, videoIdx, target, AVSEEK_FLAG_BACKWARD);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool gotFrame = false;
	int decoded = 0;

	while (!gotFrame && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index != videoIdx) {
			av_packet_unref(pkt);
			continue;
		}
		if (avcodec_send_packet(ctx, pkt) >= 0) {
			while (avcodec_receive_frame(ctx, frame) >= 0) {
				decoded++;
				/* Accept the frame at/after the target, or bail out
				 * after a while so badly indexed files still work. */
				int64_t pts = frame->best_effort_timestamp;
				if (pts == AV_NOPTS_VALUE || pts >= target || decoded > 300) {
					gotFrame = true;
					break;
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (gotFrame && frame->width > 0) {
		int outW = frame->width;
		int outH = frame->height;
		if (maxWidth > 0 && outW > maxWidth) {
			outH = outH * maxWidth / outW;
			outW = maxWidth;
		}
		outW &= ~1;
		outH &= ~1;
		if (outH < 2)
			outH = 2;

		SwsContext *sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, outW,
						 outH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (sws) {
			QImage img(outW, outH, QImage::Format_RGBA8888);
			uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
			int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
			sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
			sws_freeContext(sws);
			result = img;
		}
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return result;
}

QVector<QImage> extractStrip(const QString &path, qint64 startMs, qint64 spanMs, int count, int maxWidth,
			     const std::function<bool()> &cancelled)
{
	QVector<QImage> result;
	if (count <= 0 || spanMs <= 0)
		return result;

	AVFormatContext *fmt = open_input(path);
	if (!fmt)
		return result;

	int videoIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (videoIdx < 0) {
		avformat_close_input(&fmt);
		return result;
	}
	AVStream *st = fmt->streams[videoIdx];

	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (ctx) {
		ctx->thread_count = 0; /* auto */
	}
	if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, dec, nullptr) < 0) {
		if (ctx)
			avcodec_free_context(&ctx);
		avformat_close_input(&fmt);
		return result;
	}
	/* strip tiles are tiny — decode as cheaply as possible */
	ctx->skip_loop_filter = AVDISCARD_ALL;
	ctx->skip_frame = AVDISCARD_NONREF;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *hold = av_frame_alloc();
	SwsContext *sws = nullptr;
	int swsW = 0, swsH = 0;
	AVPixelFormat swsFmt = AV_PIX_FMT_NONE;
	qint64 posMs = -1; /* last decoded frame time; -1 = unknown */

	auto toImage = [&](const AVFrame *f) -> QImage {
		int outW = f->width;
		int outH = f->height;
		if (maxWidth > 0 && outW > maxWidth) {
			outH = outH * maxWidth / outW;
			outW = maxWidth;
		}
		outW &= ~1;
		outH &= ~1;
		if (outH < 2)
			outH = 2;
		if (!sws || swsW != f->width || swsH != f->height || swsFmt != (AVPixelFormat)f->format) {
			if (sws)
				sws_freeContext(sws);
			sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, outW, outH,
					     AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
			swsW = f->width;
			swsH = f->height;
			swsFmt = (AVPixelFormat)f->format;
		}
		if (!sws)
			return QImage();
		QImage img(outW, outH, QImage::Format_RGBA8888);
		uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
		int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
		sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);
		return img;
	};

	for (int i = 0; i < count; i++) {
		if (cancelled && cancelled()) {
			result.clear();
			break;
		}

		qint64 targetMs = startMs + spanMs * (2 * i + 1) / (2 * count);

		/* Adjacent targets often share a GOP: rolling forward beats
		 * seeking back to the same keyframe and re-decoding it. */
		bool rollForward = posMs >= 0 && targetMs > posMs && targetMs - posMs < 3000;
		if (!rollForward) {
			int64_t ts = av_rescale_q(targetMs, AVRational{1, 1000}, st->time_base);
			av_seek_frame(fmt, videoIdx, ts, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(ctx);
			posMs = -1;
		}

		bool got = false;
		bool haveHold = false;
		int guard = 0;
		while (!got && guard++ < 1200) {
			if (av_read_frame(fmt, pkt) < 0)
				break;
			if (pkt->stream_index != videoIdx) {
				av_packet_unref(pkt);
				continue;
			}
			if (avcodec_send_packet(ctx, pkt) >= 0) {
				while (avcodec_receive_frame(ctx, frame) >= 0) {
					int64_t pts = frame->best_effort_timestamp;
					qint64 ms = pts != AV_NOPTS_VALUE
							    ? av_rescale_q(pts, st->time_base, AVRational{1, 1000})
							    : targetMs;
					posMs = ms;
					av_frame_unref(hold);
					av_frame_ref(hold, frame);
					haveHold = true;
					av_frame_unref(frame);
					if (ms >= targetMs) {
						got = true;
						break;
					}
				}
			}
			av_packet_unref(pkt);
		}

		result.push_back(haveHold ? toImage(hold) : QImage());
	}

	if (sws)
		sws_freeContext(sws);
	av_frame_free(&hold);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return result;
}

} // namespace VideoProbe
