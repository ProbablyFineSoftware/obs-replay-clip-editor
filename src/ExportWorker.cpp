/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ExportWorker.hpp"

#include <QFile>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

namespace {

QString averr(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return QString::fromUtf8(buf);
}

AVPixelFormat pick_pix_fmt(const AVCodec *enc, AVPixelFormat decoded)
{
	const AVPixelFormat *fmts = nullptr;
	int count = 0;
	int ret = avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0,
					       reinterpret_cast<const void **>(&fmts), &count);
	if (ret < 0 || !fmts || count <= 0)
		return decoded;
	for (int i = 0; i < count; i++) {
		if (fmts[i] == decoded)
			return decoded;
	}
	/* Prefer common 4:2:0 formats over the encoder's first entry, which for
	 * hardware encoders is often a hwframes format we can't produce. */
	for (int i = 0; i < count; i++) {
		if (fmts[i] == AV_PIX_FMT_YUV420P || fmts[i] == AV_PIX_FMT_NV12)
			return fmts[i];
	}
	return fmts[0];
}

/* Map UI rate control settings onto encoder-family specific options. */
void apply_rate_control(AVCodecContext *enc, AVDictionary **opts, const ExportSettings &s)
{
	const QString id = s.encoderName;
	const int64_t bitrate = (int64_t)s.bitrateKbps * 1000;
	const QByteArray cqStr = QByteArray::number(s.cq);

	if (id.contains(QStringLiteral("nvenc"))) {
		av_dict_set(opts, "preset", "p5", 0);
		av_dict_set(opts, "tune", "hq", 0);
		/* Match OBS's NVENC configuration so quality per bit is
		 * comparable: B-frames, adaptive quantization, multipass. */
		enc->max_b_frames = 2;
		av_dict_set(opts, "spatial_aq", "1", 0);
		av_dict_set(opts, "aq-strength", "8", 0);
		if (id.startsWith(QStringLiteral("h264")))
			av_dict_set(opts, "profile", "high", 0);
		if (s.rateControl == QStringLiteral("CQP")) {
			av_dict_set(opts, "rc", "constqp", 0);
			av_dict_set(opts, "qp", cqStr.constData(), 0);
		} else if (s.rateControl == QStringLiteral("CBR")) {
			av_dict_set(opts, "rc", "cbr", 0);
			av_dict_set(opts, "multipass", "qres", 0);
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate;
			enc->rc_buffer_size = (int)bitrate;
		} else {
			av_dict_set(opts, "rc", "vbr", 0);
			av_dict_set(opts, "multipass", "qres", 0);
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate * 3 / 2;
		}
	} else if (id.contains(QStringLiteral("amf"))) {
		av_dict_set(opts, "quality", "quality", 0);
		if (s.rateControl == QStringLiteral("CQP")) {
			av_dict_set(opts, "rc", "cqp", 0);
			av_dict_set(opts, "qp_i", cqStr.constData(), 0);
			av_dict_set(opts, "qp_p", cqStr.constData(), 0);
		} else if (s.rateControl == QStringLiteral("CBR")) {
			av_dict_set(opts, "rc", "cbr", 0);
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate;
		} else {
			av_dict_set(opts, "rc", "vbr_peak", 0);
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate * 3 / 2;
		}
	} else if (id.contains(QStringLiteral("qsv"))) {
		if (s.rateControl == QStringLiteral("CQP")) {
			enc->global_quality = s.cq;
		} else if (s.rateControl == QStringLiteral("CBR")) {
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate;
		} else {
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate * 3 / 2;
		}
	} else if (id == QStringLiteral("libx264") || id == QStringLiteral("libx265")) {
		av_dict_set(opts, "preset", "veryfast", 0);
		if (s.rateControl == QStringLiteral("CQP")) {
			av_dict_set(opts, "crf", cqStr.constData(), 0);
		} else if (s.rateControl == QStringLiteral("CBR")) {
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate;
			enc->rc_buffer_size = (int)bitrate;
		} else {
			enc->bit_rate = bitrate;
			enc->rc_max_rate = bitrate * 3 / 2;
		}
	} else {
		/* svt-av1, aom, openh264, ... */
		if (s.rateControl == QStringLiteral("CQP"))
			av_dict_set(opts, "crf", cqStr.constData(), 0);
		else
			enc->bit_rate = bitrate;
	}
}

struct AudioInput {
	int streamIndex = -1;
	AVCodecContext *dec = nullptr;
	AVFilterContext *src = nullptr;
	bool done = false;
};

struct Pipeline {
	AVFormatContext *inFmt = nullptr;
	AVFormatContext *outFmt = nullptr;

	int videoIdx = -1;
	AVCodecContext *vdec = nullptr;
	AVCodecContext *venc = nullptr;
	AVStream *vOutStream = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *swsFrame = nullptr;

	std::vector<AudioInput> audio;
	AVFilterGraph *graph = nullptr;
	AVFilterContext *sink = nullptr;
	AVCodecContext *aenc = nullptr;
	AVStream *aOutStream = nullptr;

	AVPacket *pkt = nullptr;
	AVPacket *encPkt = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *filtFrame = nullptr;

	~Pipeline()
	{
		if (sws)
			sws_freeContext(sws);
		av_frame_free(&swsFrame);
		av_frame_free(&frame);
		av_frame_free(&filtFrame);
		av_packet_free(&pkt);
		av_packet_free(&encPkt);
		avcodec_free_context(&vdec);
		avcodec_free_context(&venc);
		avcodec_free_context(&aenc);
		for (auto &a : audio)
			avcodec_free_context(&a.dec);
		avfilter_graph_free(&graph);
		if (outFmt) {
			if (!(outFmt->oformat->flags & AVFMT_NOFILE) && outFmt->pb)
				avio_closep(&outFmt->pb);
			avformat_free_context(outFmt);
		}
		avformat_close_input(&inFmt);
	}
};

} // namespace

ExportWorker::ExportWorker(QString inputPath, QString outputPath, qint64 inMs, qint64 outMs,
			   QVector<int> audioStreamIndices, ExportSettings settings, QObject *parent)
	: QThread(parent),
	  inputPath(std::move(inputPath)),
	  outputPath(std::move(outputPath)),
	  inMs(inMs),
	  outMs(outMs),
	  audioStreamIndices(std::move(audioStreamIndices)),
	  settings(std::move(settings))
{
}

QVector<EncoderChoice> ExportWorker::availableEncoders()
{
	static QVector<EncoderChoice> cached;
	static bool probed = false;
	if (probed)
		return cached;
	probed = true;

	const struct {
		const char *id;
		const char *name;
	} candidates[] = {
		{"h264_nvenc", "NVIDIA NVENC H.264"},
		{"hevc_nvenc", "NVIDIA NVENC HEVC"},
		{"av1_nvenc", "NVIDIA NVENC AV1"},
		{"h264_amf", "AMD HW H.264"},
		{"hevc_amf", "AMD HW HEVC"},
		{"av1_amf", "AMD HW AV1"},
		{"h264_qsv", "Intel QuickSync H.264"},
		{"hevc_qsv", "Intel QuickSync HEVC"},
		{"av1_qsv", "Intel QuickSync AV1"},
		{"libx264", "x264 (Software H.264)"},
		{"libopenh264", "OpenH264 (Software)"},
		{"libsvtav1", "SVT-AV1 (Software)"},
	};

	for (const auto &c : candidates) {
		const AVCodec *enc = avcodec_find_encoder_by_name(c.id);
		if (!enc)
			continue;

		/* Actually try to open it — hardware encoders exist in the build
		 * even when the GPU/driver can't do them. */
		AVCodecContext *ctx = avcodec_alloc_context3(enc);
		if (!ctx)
			continue;
		ctx->width = 1280;
		ctx->height = 720;
		ctx->time_base = {1, 60};
		ctx->framerate = {60, 1};
		ctx->pix_fmt = pick_pix_fmt(enc, AV_PIX_FMT_YUV420P);
		bool ok = avcodec_open2(ctx, enc, nullptr) >= 0;
		avcodec_free_context(&ctx);

		if (ok)
			cached.push_back({QString::fromUtf8(c.id), QString::fromUtf8(c.name)});
	}
	return cached;
}

void ExportWorker::run()
{
	Pipeline p;
	int ret;

	auto fail = [&](const QString &msg) {
		QFile::remove(outputPath);
		emit exportFinished(false, msg);
	};

	/* ---- Input ---- */
	if ((ret = avformat_open_input(&p.inFmt, inputPath.toUtf8().constData(), nullptr, nullptr)) < 0)
		return fail(QStringLiteral("Failed to open input: %1").arg(averr(ret)));
	if ((ret = avformat_find_stream_info(p.inFmt, nullptr)) < 0)
		return fail(QStringLiteral("Failed to read streams: %1").arg(averr(ret)));

	p.videoIdx = av_find_best_stream(p.inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (p.videoIdx < 0)
		return fail(QStringLiteral("No video stream in file"));
	AVStream *vst = p.inFmt->streams[p.videoIdx];

	const AVCodec *vdecCodec = avcodec_find_decoder(vst->codecpar->codec_id);
	p.vdec = avcodec_alloc_context3(vdecCodec);
	avcodec_parameters_to_context(p.vdec, vst->codecpar);
	p.vdec->thread_count = 0;
	if ((ret = avcodec_open2(p.vdec, vdecCodec, nullptr)) < 0)
		return fail(QStringLiteral("Failed to open video decoder: %1").arg(averr(ret)));

	for (int idx : audioStreamIndices) {
		if (idx < 0 || idx >= (int)p.inFmt->nb_streams)
			continue;
		AVStream *ast = p.inFmt->streams[idx];
		if (ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		const AVCodec *adec = avcodec_find_decoder(ast->codecpar->codec_id);
		AudioInput ai;
		ai.streamIndex = idx;
		ai.dec = avcodec_alloc_context3(adec);
		avcodec_parameters_to_context(ai.dec, ast->codecpar);
		if (avcodec_open2(ai.dec, adec, nullptr) < 0) {
			avcodec_free_context(&ai.dec);
			continue;
		}
		p.audio.push_back(ai);
	}

	/* ---- Video encoder ---- */
	const AVCodec *vencCodec = avcodec_find_encoder_by_name(settings.encoderName.toUtf8().constData());
	if (!vencCodec)
		return fail(QStringLiteral("Encoder not available: %1").arg(settings.encoderName));

	p.venc = avcodec_alloc_context3(vencCodec);
	int outW = p.vdec->width;
	int outH = p.vdec->height;
	if (settings.scaleHeight > 0 && settings.scaleHeight < p.vdec->height) {
		outH = settings.scaleHeight & ~1;
		outW = (int)((int64_t)p.vdec->width * outH / p.vdec->height) & ~1;
	}
	p.venc->width = outW;
	p.venc->height = outH;
	p.venc->sample_aspect_ratio = p.vdec->sample_aspect_ratio;
	p.venc->time_base = vst->time_base;
	AVRational fr = vst->avg_frame_rate.num > 0 ? vst->avg_frame_rate : AVRational{60, 1};
	p.venc->framerate = fr;
	p.venc->gop_size = (int)(av_q2d(fr) * 2.0);
	p.venc->pix_fmt = pick_pix_fmt(vencCodec, p.vdec->pix_fmt);
	p.venc->color_range = p.vdec->color_range;
	p.venc->color_primaries = p.vdec->color_primaries;
	p.venc->color_trc = p.vdec->color_trc;
	p.venc->colorspace = p.vdec->colorspace;

	AVDictionary *vopts = nullptr;
	apply_rate_control(p.venc, &vopts, settings);

	/* ---- Output container ---- */
	if ((ret = avformat_alloc_output_context2(&p.outFmt, nullptr, nullptr, outputPath.toUtf8().constData())) < 0)
		return fail(QStringLiteral("Failed to create output: %1").arg(averr(ret)));

	if (p.outFmt->oformat->flags & AVFMT_GLOBALHEADER)
		p.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if ((ret = avcodec_open2(p.venc, vencCodec, &vopts)) < 0) {
		av_dict_free(&vopts);
		return fail(QStringLiteral("Failed to open encoder %1: %2").arg(settings.encoderName, averr(ret)));
	}
	av_dict_free(&vopts);

	p.vOutStream = avformat_new_stream(p.outFmt, nullptr);
	avcodec_parameters_from_context(p.vOutStream->codecpar, p.venc);
	p.vOutStream->time_base = p.venc->time_base;
	p.vOutStream->avg_frame_rate = fr;

	/* ---- Audio filter graph + encoder ---- */
	const bool hasAudio = !p.audio.empty();
	if (hasAudio) {
		p.graph = avfilter_graph_alloc();

		QStringList inNames;
		for (size_t i = 0; i < p.audio.size(); i++) {
			AudioInput &ai = p.audio[i];
			AVStream *ast = p.inFmt->streams[ai.streamIndex];
			char layout[128];
			av_channel_layout_describe(&ai.dec->ch_layout, layout, sizeof(layout));
			QByteArray args =
				QStringLiteral("time_base=%1/%2:sample_rate=%3:sample_fmt=%4:channel_layout=%5")
					.arg(ast->time_base.num)
					.arg(ast->time_base.den)
					.arg(ai.dec->sample_rate)
					.arg(QString::fromUtf8(av_get_sample_fmt_name(ai.dec->sample_fmt)))
					.arg(QString::fromUtf8(layout))
					.toUtf8();
			QByteArray name = QStringLiteral("in%1").arg(i).toUtf8();
			ret = avfilter_graph_create_filter(&ai.src, avfilter_get_by_name("abuffer"), name.constData(),
							   args.constData(), nullptr, p.graph);
			if (ret < 0)
				return fail(QStringLiteral("Audio filter setup failed: %1").arg(averr(ret)));
			inNames << QString::fromUtf8(name);
		}

		ret = avfilter_graph_create_filter(&p.sink, avfilter_get_by_name("abuffersink"), "out", nullptr,
						   nullptr, p.graph);
		if (ret < 0)
			return fail(QStringLiteral("Audio sink setup failed: %1").arg(averr(ret)));

		static const enum AVSampleFormat sinkFmts[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE};
		av_opt_set_int_list(p.sink, "sample_fmts", sinkFmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

		QString desc;
		if (p.audio.size() > 1) {
			for (const QString &n : inNames)
				desc += QStringLiteral("[%1]").arg(n);
			desc += QStringLiteral("amix=inputs=%1:normalize=0,").arg(p.audio.size());
		} else {
			desc = QStringLiteral("[%1]").arg(inNames[0]);
		}
		desc += QStringLiteral("aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[out]");

		AVFilterInOut *inputs = nullptr;
		AVFilterInOut *outputs = nullptr;
		ret = avfilter_graph_parse2(p.graph, desc.toUtf8().constData(), &inputs, &outputs);
		if (ret < 0)
			return fail(QStringLiteral("Audio graph parse failed: %1").arg(averr(ret)));

		/* Wire named abuffer sources to parsed graph inputs, and the
		 * parsed output to our sink. */
		int i = 0;
		for (AVFilterInOut *cur = inputs; cur; cur = cur->next, i++) {
			int srcIdx = 0;
			sscanf(cur->name ? cur->name : "in0", "in%d", &srcIdx);
			if (srcIdx < 0 || srcIdx >= (int)p.audio.size())
				srcIdx = i;
			ret = avfilter_link(p.audio[srcIdx].src, 0, cur->filter_ctx, cur->pad_idx);
			if (ret < 0)
				break;
		}
		if (ret >= 0 && outputs)
			ret = avfilter_link(outputs->filter_ctx, outputs->pad_idx, p.sink, 0);
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		if (ret < 0)
			return fail(QStringLiteral("Audio graph link failed: %1").arg(averr(ret)));

		if ((ret = avfilter_graph_config(p.graph, nullptr)) < 0)
			return fail(QStringLiteral("Audio graph config failed: %1").arg(averr(ret)));

		const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
		p.aenc = avcodec_alloc_context3(aac);
		p.aenc->sample_rate = 48000;
		p.aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
		av_channel_layout_default(&p.aenc->ch_layout, 2);
		p.aenc->bit_rate = 192000;
		p.aenc->time_base = {1, 48000};
		if (p.outFmt->oformat->flags & AVFMT_GLOBALHEADER)
			p.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		if ((ret = avcodec_open2(p.aenc, aac, nullptr)) < 0)
			return fail(QStringLiteral("Failed to open AAC encoder: %1").arg(averr(ret)));

		av_buffersink_set_frame_size(p.sink, p.aenc->frame_size);

		p.aOutStream = avformat_new_stream(p.outFmt, nullptr);
		avcodec_parameters_from_context(p.aOutStream->codecpar, p.aenc);
		p.aOutStream->time_base = p.aenc->time_base;
	}

	/* ---- Open file, write header ---- */
	if (!(p.outFmt->oformat->flags & AVFMT_NOFILE)) {
		if ((ret = avio_open(&p.outFmt->pb, outputPath.toUtf8().constData(), AVIO_FLAG_WRITE)) < 0)
			return fail(QStringLiteral("Cannot write output file: %1").arg(averr(ret)));
	}
	AVDictionary *muxOpts = nullptr;
	if (settings.container != QStringLiteral("mkv"))
		av_dict_set(&muxOpts, "movflags", "+faststart", 0);
	ret = avformat_write_header(p.outFmt, &muxOpts);
	av_dict_free(&muxOpts);
	if (ret < 0)
		return fail(QStringLiteral("Failed to write header: %1").arg(averr(ret)));

	/* ---- Seek near the in point ---- */
	av_seek_frame(p.inFmt, -1, inMs * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD);

	p.pkt = av_packet_alloc();
	p.encPkt = av_packet_alloc();
	p.frame = av_frame_alloc();
	p.filtFrame = av_frame_alloc();

	const int64_t inVpts = av_rescale_q(inMs, {1, 1000}, vst->time_base);
	int64_t audioSamplesOut = 0;
	bool videoDone = false;
	int lastProgress = -1;
	int64_t lastVideoPts = AV_NOPTS_VALUE;

	auto writeEncoded = [&](AVCodecContext *enc, AVStream *stream) -> int {
		int r;
		while ((r = avcodec_receive_packet(enc, p.encPkt)) >= 0) {
			av_packet_rescale_ts(p.encPkt, enc->time_base, stream->time_base);
			p.encPkt->stream_index = stream->index;
			r = av_interleaved_write_frame(p.outFmt, p.encPkt);
			av_packet_unref(p.encPkt);
			if (r < 0)
				return r;
		}
		return (r == AVERROR(EAGAIN) || r == AVERROR_EOF) ? 0 : r;
	};

	auto drainSink = [&](bool flushing) -> int {
		int r;
		while (true) {
			r = av_buffersink_get_frame_flags(p.sink, p.filtFrame,
							  flushing ? 0 : AV_BUFFERSINK_FLAG_NO_REQUEST);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				return 0;
			if (r < 0)
				return r;
			/* Rebase audio onto a clean timeline starting at zero. */
			p.filtFrame->pts = audioSamplesOut;
			audioSamplesOut += p.filtFrame->nb_samples;
			r = avcodec_send_frame(p.aenc, p.filtFrame);
			av_frame_unref(p.filtFrame);
			if (r < 0)
				return r;
			if ((r = writeEncoded(p.aenc, p.aOutStream)) < 0)
				return r;
		}
	};

	auto sendVideoFrame = [&](AVFrame *f) -> int {
		AVFrame *toSend = f;
		bool needsConvert =
			f && (f->format != p.venc->pix_fmt || f->width != p.venc->width || f->height != p.venc->height);
		if (needsConvert) {
			if (!p.sws) {
				p.sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, p.venc->width,
						       p.venc->height, p.venc->pix_fmt, SWS_BICUBIC, nullptr, nullptr,
						       nullptr);
				p.swsFrame = av_frame_alloc();
				p.swsFrame->width = p.venc->width;
				p.swsFrame->height = p.venc->height;
				p.swsFrame->format = p.venc->pix_fmt;
				av_frame_get_buffer(p.swsFrame, 0);
			}
			sws_scale(p.sws, f->data, f->linesize, 0, f->height, p.swsFrame->data, p.swsFrame->linesize);
			p.swsFrame->pts = f->pts;
			toSend = p.swsFrame;
		}
		int r = avcodec_send_frame(p.venc, toSend);
		if (r < 0)
			return r;
		return writeEncoded(p.venc, p.vOutStream);
	};

	/* ---- Main demux/transcode loop ---- */
	bool readEof = false;
	while (!readEof) {
		if (cancelled) {
			emit exportFinished(false, QStringLiteral("cancelled"));
			QFile::remove(outputPath);
			return;
		}

		bool audioAllDone = true;
		for (auto &a : p.audio)
			if (!a.done)
				audioAllDone = false;
		if (videoDone && audioAllDone)
			break;

		ret = av_read_frame(p.inFmt, p.pkt);
		if (ret == AVERROR_EOF) {
			readEof = true;
		} else if (ret < 0) {
			return fail(QStringLiteral("Read error: %1").arg(averr(ret)));
		}

		if (!readEof && p.pkt->stream_index == p.videoIdx && !videoDone) {
			ret = avcodec_send_packet(p.vdec, p.pkt);
			if (ret >= 0) {
				while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
					int64_t pts = p.frame->best_effort_timestamp;
					if (pts == AV_NOPTS_VALUE)
						pts = lastVideoPts == AV_NOPTS_VALUE ? inVpts : lastVideoPts + 1;
					lastVideoPts = pts;
					qint64 ms = av_rescale_q(pts, vst->time_base, {1, 1000});
					if (ms < inMs) {
						av_frame_unref(p.frame);
						continue;
					}
					if (ms > outMs) {
						videoDone = true;
						av_frame_unref(p.frame);
						break;
					}
					p.frame->pts = pts - inVpts;
					p.frame->pict_type = AV_PICTURE_TYPE_NONE;
					int r = sendVideoFrame(p.frame);
					av_frame_unref(p.frame);
					if (r < 0)
						return fail(QStringLiteral("Video encode failed: %1").arg(averr(r)));

					int prog = (int)((ms - inMs) * 100 / std::max<qint64>(outMs - inMs, 1));
					if (prog != lastProgress) {
						lastProgress = prog;
						emit progressChanged(std::clamp(prog, 0, 100));
					}
				}
			}
		} else if (!readEof && hasAudio) {
			for (auto &a : p.audio) {
				if (a.streamIndex != p.pkt->stream_index || a.done)
					continue;
				AVStream *ast = p.inFmt->streams[a.streamIndex];
				ret = avcodec_send_packet(a.dec, p.pkt);
				if (ret < 0)
					break;
				while (avcodec_receive_frame(a.dec, p.frame) >= 0) {
					int64_t pts = p.frame->best_effort_timestamp;
					qint64 ms = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, ast->time_base, {1, 1000})
									  : inMs;
					qint64 frameDurMs =
						p.frame->nb_samples * 1000LL / std::max(p.frame->sample_rate, 1);
					if (ms + frameDurMs < inMs) {
						av_frame_unref(p.frame);
						continue;
					}
					if (ms > outMs) {
						a.done = true;
						av_buffersrc_add_frame_flags(a.src, nullptr, 0);
						av_frame_unref(p.frame);
						break;
					}
					int r = av_buffersrc_add_frame_flags(a.src, p.frame,
									     AV_BUFFERSRC_FLAG_KEEP_REF);
					av_frame_unref(p.frame);
					if (r < 0)
						return fail(
							QStringLiteral("Audio filter feed failed: %1").arg(averr(r)));
				}
				if ((ret = drainSink(false)) < 0)
					return fail(QStringLiteral("Audio encode failed: %1").arg(averr(ret)));
				break;
			}
		}
		av_packet_unref(p.pkt);

		if (readEof)
			break;
	}

	/* ---- Flush everything ---- */
	if (!videoDone) {
		avcodec_send_packet(p.vdec, nullptr);
		while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
			int64_t pts = p.frame->best_effort_timestamp;
			qint64 ms = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, vst->time_base, {1, 1000}) : inMs;
			if (ms >= inMs && ms <= outMs) {
				p.frame->pts = pts - inVpts;
				sendVideoFrame(p.frame);
			}
			av_frame_unref(p.frame);
		}
	}
	avcodec_send_frame(p.venc, nullptr);
	writeEncoded(p.venc, p.vOutStream);

	if (hasAudio) {
		for (auto &a : p.audio) {
			if (!a.done) {
				avcodec_send_packet(a.dec, nullptr);
				while (avcodec_receive_frame(a.dec, p.frame) >= 0) {
					av_buffersrc_add_frame_flags(a.src, p.frame, AV_BUFFERSRC_FLAG_KEEP_REF);
					av_frame_unref(p.frame);
				}
				av_buffersrc_add_frame_flags(a.src, nullptr, 0);
				a.done = true;
			}
		}
		if ((ret = drainSink(true)) < 0)
			return fail(QStringLiteral("Audio flush failed: %1").arg(averr(ret)));
		avcodec_send_frame(p.aenc, nullptr);
		writeEncoded(p.aenc, p.aOutStream);
	}

	if ((ret = av_write_trailer(p.outFmt)) < 0)
		return fail(QStringLiteral("Failed to finalize file: %1").arg(averr(ret)));

	emit progressChanged(100);
	emit exportFinished(true, outputPath);
}
